#include "ac_encoder.h"
#include "ac_utils.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ac {

EncodedColumn AdaptiveDictionaryEncoder::encode(uint32_t colIndex1Based,
                                                 const std::vector<std::string>& columnValues) {
    EncodedColumn column;
    column.columnIndex1Based = colIndex1Based;
    column.isString          = is_string_column(columnValues);

    // Non-string columns are stored as raw values; no dictionary encoding.
    if (!column.isString) {
        column.dictionaryEnabled = false;
        column.rawValues         = columnValues;
        return column;
    }

    column.dictionaryEnabled = true;

    // --- Sequence state ---------------------------------------------------
    // These variables track the current differential sequence.
    // They reset whenever we start a new local (base) page.
    std::vector<std::string>              cumulativeDictionary;
    std::unordered_map<std::string, uint32_t> cumulativeIndex;
    uint8_t  currentOffsetWidth       = 1;
    uint32_t diffCount                = 0;  // differential pages in current sequence
    uint64_t diffDictionaryBytesTotal = 0;  // total dict bytes across those diff pages
    // ----------------------------------------------------------------------

    const size_t totalRows = columnValues.size();

    for (size_t pageStart = 0; pageStart < totalRows; pageStart += kDefaultPageSize) {

        const size_t pageEnd  = std::min(pageStart + static_cast<size_t>(kDefaultPageSize),
                                         totalRows);
        const size_t pageRows = pageEnd - pageStart;

        // C1: no page copy -- use iterators into columnValues directly.
        auto rangeBegin = columnValues.cbegin() + static_cast<long>(pageStart);
        auto rangeEnd   = columnValues.cbegin() + static_cast<long>(pageEnd);

        // ------------------------------------------------------------------
        // Step 1: Cardinality check.
        // If more than 80 % of the page's values are distinct, differential
        // encoding will not help. Reset the sequence state so this page
        // becomes the start of a new local sequence.
        // ------------------------------------------------------------------
        std::unordered_set<std::string> pageSet(rangeBegin, rangeEnd);
        const double distinctRatio = static_cast<double>(pageSet.size())
                                     / static_cast<double>(pageRows);

        if (distinctRatio > kDistinctRatioDisable) {
            cumulativeDictionary.clear();
            cumulativeIndex.clear();
            currentOffsetWidth       = 1;
            diffCount                = 0;
            diffDictionaryBytesTotal = 0;
        }

        // ------------------------------------------------------------------
        // Step 2: Build the local dictionary (sorted distinct values).
        // C2: localIndex and localOffsets are deferred to the if (chooseLocal)
        // branch -- on differential pages (the common case) this work is skipped.
        // ------------------------------------------------------------------
        std::vector<std::string> localDict(pageSet.begin(), pageSet.end());
        std::sort(localDict.begin(), localDict.end());

        // ------------------------------------------------------------------
        // Step 3: Build the differential dictionary (new values only) and
        // count how many rows in this page repeat values from prior pages.
        // C1: iterate the range, not a copied page vector.
        // ------------------------------------------------------------------
        uint32_t repeatingRows = 0;
        for (auto it = rangeBegin; it != rangeEnd; ++it) {
            if (cumulativeIndex.count(*it) > 0) ++repeatingRows;
        }
        const double repeatRatio = (pageRows == 0)
            ? 0.0
            : static_cast<double>(repeatingRows) / static_cast<double>(pageRows);

        // diffDict contains values not yet in the cumulative dictionary.
        // Iterating the already-sorted localDict keeps diffDict sorted too.
        std::vector<std::string> diffDict;
        diffDict.reserve(localDict.size());
        for (const std::string& v : localDict) {
            if (cumulativeIndex.count(v) == 0) diffDict.push_back(v);
        }

        // ------------------------------------------------------------------
        // Step 4: Size estimates used by the cost function.
        // ------------------------------------------------------------------
        const uint32_t diffCardinality  = static_cast<uint32_t>(cumulativeDictionary.size()
                                                                  + diffDict.size());
        const uint8_t  diffOffsetWidth  = width_for_cardinality(diffCardinality);
        const uint8_t  localOffsetWidth = width_for_cardinality(
                                              static_cast<uint32_t>(localDict.size()));

        const uint64_t localDictBytes   = dictionary_storage_bytes(localDict);
        const uint64_t diffDictBytes    = dictionary_storage_bytes(diffDict);
        const uint64_t localOffsetBytes = static_cast<uint64_t>(pageRows) * localOffsetWidth;
        const uint64_t diffOffsetBytes  = static_cast<uint64_t>(pageRows) * diffOffsetWidth;
        const uint64_t localPageSize    = localDictBytes  + localOffsetBytes;
        const uint64_t diffPageSize     = diffDictBytes   + diffOffsetBytes;

        // ------------------------------------------------------------------
        // Step 5: Decide between local and differential encoding.
        //
        // Rule 1 -- always use local for the first page of a sequence
        //           (no prior context to base differential encoding on).
        // Rule 2 -- if fewer than 10 % of rows repeat values from prior pages,
        //           differential encoding saves little; use local.
        // Rule 3 -- if the offset width would not increase, always use
        //           differential (cost function not needed).
        // Rule 4 -- if the offset width would increase, evaluate the cost
        //           function (Equation 1 in the paper) to predict whether
        //           the future overhead of larger offsets outweighs the
        //           current-page savings from differential encoding.
        // ------------------------------------------------------------------
        bool chooseLocal = cumulativeDictionary.empty();  // Rule 1

        if (!chooseLocal) {
            if (repeatRatio < kMinCrossPageRepeatRatio) {
                // Rule 2
                chooseLocal = true;
            } else if (diffOffsetWidth == currentOffsetWidth) {
                // Rule 3 -- no width increase, always differential
                chooseLocal = false;
            } else {
                // Rule 4 -- cost function
                // diffCount == 0 makes the first term zero, so diffAvg has no effect.
                const double diffAvg = (diffCount == 0)
                    ? 0.0
                    : static_cast<double>(diffDictionaryBytesTotal)
                      / static_cast<double>(diffCount);

                // diffcount * (difOffsetSize - diffavg) - locOffsetSize
                //   - (locPageSize - difPageSize) > 0
                const double lhs =
                    static_cast<double>(diffCount)
                        * (static_cast<double>(diffOffsetBytes) - diffAvg)
                    - static_cast<double>(localOffsetBytes)
                    - (static_cast<double>(localPageSize) - static_cast<double>(diffPageSize));

                chooseLocal = (lhs > 0.0);
            }
        }

        // ------------------------------------------------------------------
        // Step 6: Encode the page and update sequence state.
        // ------------------------------------------------------------------
        EncodedPage encoded;
        encoded.rowCount = static_cast<uint32_t>(pageRows);

        // C1: pageMin/pageMax via minmax_element on the range (no page vector).
        auto mm = std::minmax_element(rangeBegin, rangeEnd);
        encoded.pageMin = *mm.first;
        encoded.pageMax = *mm.second;

        if (chooseLocal) {
            // C2: build localIndex and localOffsets here, only when needed.
            // C1: localOffsets loop uses the range, not a page vector.
            std::unordered_map<std::string, uint32_t> localIndex;
            localIndex.reserve(localDict.size() * 2 + 1);
            for (uint32_t i = 0; i < static_cast<uint32_t>(localDict.size()); ++i) {
                localIndex[localDict[i]] = i;
            }

            std::vector<uint32_t> localOffsets;
            localOffsets.reserve(pageRows);
            for (auto it = rangeBegin; it != rangeEnd; ++it) {
                localOffsets.push_back(localIndex[*it]);
            }

            encoded.isLocal     = true;
            encoded.offsetWidth = localOffsetWidth;
            encoded.dictionary  = localDict;
            encoded.offsets     = localOffsets;

            std::pair<std::string, std::string> dm = min_max(localDict);
            encoded.diffMin = dm.first;
            encoded.diffMax = dm.second;

            // This local page becomes the new base of the sequence.
            cumulativeDictionary = localDict;
            cumulativeIndex.clear();
            cumulativeIndex.reserve(cumulativeDictionary.size() * 2 + 1);
            for (uint32_t i = 0; i < static_cast<uint32_t>(cumulativeDictionary.size()); ++i) {
                cumulativeIndex[cumulativeDictionary[i]] = i;
            }
            currentOffsetWidth       = width_for_cardinality(
                                           static_cast<uint32_t>(cumulativeDictionary.size()));
            diffCount                = 0;
            diffDictionaryBytesTotal = 0;

        } else {
            encoded.isLocal     = false;
            encoded.offsetWidth = diffOffsetWidth;
            encoded.dictionary  = diffDict;

            std::pair<std::string, std::string> dm = min_max(diffDict);
            encoded.diffMin = dm.first;
            encoded.diffMax = dm.second;

            // Append new values to the cumulative dictionary first,
            // then encode every row using its cumulative index.
            for (const std::string& v : diffDict) {
                cumulativeIndex[v] = static_cast<uint32_t>(cumulativeDictionary.size());
                cumulativeDictionary.push_back(v);
            }

            std::vector<uint32_t> diffOffsets;
            diffOffsets.reserve(pageRows);
            // C1: iterate the range, not a page vector.
            for (auto it = rangeBegin; it != rangeEnd; ++it) {
                diffOffsets.push_back(cumulativeIndex[*it]);
            }
            encoded.offsets = diffOffsets;

            currentOffsetWidth        = width_for_cardinality(
                                            static_cast<uint32_t>(cumulativeDictionary.size()));
            ++diffCount;
            diffDictionaryBytesTotal += diffDictBytes;
        }

        column.pages.push_back(encoded);
    }

    return column;
}

} // namespace ac
