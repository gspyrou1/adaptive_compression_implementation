#include "ac_encoder_variants.h"
#include "ac_utils.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ac {

// =============================================================================
// EncoderC1 -- Optimization C1 only: eliminate the page copy.
//
// Baseline copies up to 65536 strings into a local `page` vector at the start
// of every page iteration, then uses that copy for all subsequent work.
//
// Change: remove that allocation entirely. All loops that iterated over `page`
// now iterate over the [pageStart, pageEnd) slice of `columnValues` directly.
// pageMin / pageMax are computed with std::minmax_element on the same slice.
// Everything else (localIndex, localOffsets, diffDict, cost function) is
// identical to the baseline.
// =============================================================================

EncodedColumn EncoderC1::encode(uint32_t colIndex1Based,
                                 const std::vector<std::string>& columnValues) {
    EncodedColumn column;
    column.columnIndex1Based = colIndex1Based;
    column.isString          = is_string_column(columnValues);

    if (!column.isString) {
        column.dictionaryEnabled = false;
        column.rawValues         = columnValues;
        return column;
    }

    column.dictionaryEnabled = true;

    std::vector<std::string>              cumulativeDictionary;
    std::unordered_map<std::string, uint32_t> cumulativeIndex;
    uint8_t  currentOffsetWidth       = 1;
    uint32_t diffCount                = 0;
    uint64_t diffDictionaryBytesTotal = 0;

    const size_t totalRows = columnValues.size();

    for (size_t pageStart = 0; pageStart < totalRows; pageStart += kDefaultPageSize) {

        const size_t pageEnd  = std::min(pageStart + static_cast<size_t>(kDefaultPageSize),
                                         totalRows);
        const size_t pageRows = pageEnd - pageStart;

        // C1: no page copy -- use iterators into columnValues directly.
        auto rangeBegin = columnValues.cbegin() + static_cast<long>(pageStart);
        auto rangeEnd   = columnValues.cbegin() + static_cast<long>(pageEnd);

        // Step 1: Cardinality check (same logic, range iterators instead of page).
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

        // Step 2: Build localDict, localIndex, localOffsets (unconditional, same as baseline).
        std::vector<std::string> localDict(pageSet.begin(), pageSet.end());
        std::sort(localDict.begin(), localDict.end());

        std::unordered_map<std::string, uint32_t> localIndex;
        localIndex.reserve(localDict.size() * 2 + 1);
        for (uint32_t i = 0; i < static_cast<uint32_t>(localDict.size()); ++i) {
            localIndex[localDict[i]] = i;
        }

        std::vector<uint32_t> localOffsets;
        localOffsets.reserve(pageRows);
        // C1: iterate the range, not a copied `page` vector.
        for (auto it = rangeBegin; it != rangeEnd; ++it) {
            localOffsets.push_back(localIndex[*it]);
        }

        // Step 3: repeatingRows and diffDict (same logic, range iterators).
        uint32_t repeatingRows = 0;
        // C1: iterate the range, not a copied `page` vector.
        for (auto it = rangeBegin; it != rangeEnd; ++it) {
            if (cumulativeIndex.count(*it) > 0) ++repeatingRows;
        }
        const double repeatRatio = (pageRows == 0)
            ? 0.0
            : static_cast<double>(repeatingRows) / static_cast<double>(pageRows);

        std::vector<std::string> diffDict;
        diffDict.reserve(localDict.size());
        for (const std::string& v : localDict) {
            if (cumulativeIndex.count(v) == 0) diffDict.push_back(v);
        }

        // Step 4: Size estimates (identical to baseline).
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

        // Step 5: Decision (identical to baseline).
        bool chooseLocal = cumulativeDictionary.empty();

        if (!chooseLocal) {
            if (repeatRatio < kMinCrossPageRepeatRatio) {
                chooseLocal = true;
            } else if (diffOffsetWidth == currentOffsetWidth) {
                chooseLocal = false;
            } else {
                const double diffAvg = (diffCount == 0)
                    ? 0.0
                    : static_cast<double>(diffDictionaryBytesTotal)
                      / static_cast<double>(diffCount);
                const double lhs =
                    static_cast<double>(diffCount)
                        * (static_cast<double>(diffOffsetBytes) - diffAvg)
                    - static_cast<double>(localOffsetBytes)
                    - (static_cast<double>(localPageSize) - static_cast<double>(diffPageSize));
                chooseLocal = (lhs > 0.0);
            }
        }

        // Step 6: Encode the page.
        EncodedPage encoded;
        encoded.rowCount = static_cast<uint32_t>(pageRows);

        // C1: pageMin/pageMax via minmax_element on the range (no page vector).
        auto mm = std::minmax_element(rangeBegin, rangeEnd);
        encoded.pageMin = *mm.first;
        encoded.pageMax = *mm.second;

        if (chooseLocal) {
            encoded.isLocal     = true;
            encoded.offsetWidth = localOffsetWidth;
            encoded.dictionary  = localDict;
            encoded.offsets     = localOffsets;

            std::pair<std::string, std::string> dm = min_max(localDict);
            encoded.diffMin = dm.first;
            encoded.diffMax = dm.second;

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

            for (const std::string& v : diffDict) {
                cumulativeIndex[v] = static_cast<uint32_t>(cumulativeDictionary.size());
                cumulativeDictionary.push_back(v);
            }

            std::vector<uint32_t> diffOffsets;
            diffOffsets.reserve(pageRows);
            // C1: iterate the range, not a copied `page` vector.
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

// =============================================================================
// EncoderC2 -- Optimization C2 only: defer localIndex and localOffsets.
//
// Baseline always builds localIndex (k hash insertions) and localOffsets
// (n hash lookups) before the encoding decision is made. On differential
// pages -- the common case -- this work is discarded immediately after.
//
// Change: move localIndex and localOffsets construction to inside the
// `if (chooseLocal)` branch, so they are only built when actually needed.
// The page copy and all other steps are identical to the baseline.
// =============================================================================

EncodedColumn EncoderC2::encode(uint32_t colIndex1Based,
                                 const std::vector<std::string>& columnValues) {
    EncodedColumn column;
    column.columnIndex1Based = colIndex1Based;
    column.isString          = is_string_column(columnValues);

    if (!column.isString) {
        column.dictionaryEnabled = false;
        column.rawValues         = columnValues;
        return column;
    }

    column.dictionaryEnabled = true;

    std::vector<std::string>              cumulativeDictionary;
    std::unordered_map<std::string, uint32_t> cumulativeIndex;
    uint8_t  currentOffsetWidth       = 1;
    uint32_t diffCount                = 0;
    uint64_t diffDictionaryBytesTotal = 0;

    const size_t totalRows = columnValues.size();

    for (size_t pageStart = 0; pageStart < totalRows; pageStart += kDefaultPageSize) {

        const size_t pageEnd  = std::min(pageStart + static_cast<size_t>(kDefaultPageSize),
                                         totalRows);
        const size_t pageRows = pageEnd - pageStart;

        // Baseline page copy (unchanged from baseline).
        std::vector<std::string> page(columnValues.begin() + static_cast<long>(pageStart),
                                      columnValues.begin() + static_cast<long>(pageEnd));

        // Step 1: Cardinality check (identical to baseline).
        std::unordered_set<std::string> pageSet(page.begin(), page.end());
        const double distinctRatio = static_cast<double>(pageSet.size())
                                     / static_cast<double>(pageRows);

        if (distinctRatio > kDistinctRatioDisable) {
            cumulativeDictionary.clear();
            cumulativeIndex.clear();
            currentOffsetWidth       = 1;
            diffCount                = 0;
            diffDictionaryBytesTotal = 0;
        }

        // Step 2: Build localDict only.
        // C2: localIndex and localOffsets are NOT built here.
        //     They are deferred to the if (chooseLocal) branch below.
        std::vector<std::string> localDict(pageSet.begin(), pageSet.end());
        std::sort(localDict.begin(), localDict.end());

        // Step 3: repeatingRows and diffDict (identical to baseline).
        uint32_t repeatingRows = 0;
        for (const std::string& v : page) {
            if (cumulativeIndex.count(v) > 0) ++repeatingRows;
        }
        const double repeatRatio = (pageRows == 0)
            ? 0.0
            : static_cast<double>(repeatingRows) / static_cast<double>(pageRows);

        std::vector<std::string> diffDict;
        diffDict.reserve(localDict.size());
        for (const std::string& v : localDict) {
            if (cumulativeIndex.count(v) == 0) diffDict.push_back(v);
        }

        // Step 4: Size estimates (identical to baseline).
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

        // Step 5: Decision (identical to baseline).
        bool chooseLocal = cumulativeDictionary.empty();

        if (!chooseLocal) {
            if (repeatRatio < kMinCrossPageRepeatRatio) {
                chooseLocal = true;
            } else if (diffOffsetWidth == currentOffsetWidth) {
                chooseLocal = false;
            } else {
                const double diffAvg = (diffCount == 0)
                    ? 0.0
                    : static_cast<double>(diffDictionaryBytesTotal)
                      / static_cast<double>(diffCount);
                const double lhs =
                    static_cast<double>(diffCount)
                        * (static_cast<double>(diffOffsetBytes) - diffAvg)
                    - static_cast<double>(localOffsetBytes)
                    - (static_cast<double>(localPageSize) - static_cast<double>(diffPageSize));
                chooseLocal = (lhs > 0.0);
            }
        }

        // Step 6: Encode the page.
        EncodedPage encoded;
        encoded.rowCount = static_cast<uint32_t>(pageRows);

        std::pair<std::string, std::string> pm = min_max(page);
        encoded.pageMin = pm.first;
        encoded.pageMax = pm.second;

        if (chooseLocal) {
            // C2: build localIndex and localOffsets here, only when needed.
            std::unordered_map<std::string, uint32_t> localIndex;
            localIndex.reserve(localDict.size() * 2 + 1);
            for (uint32_t i = 0; i < static_cast<uint32_t>(localDict.size()); ++i) {
                localIndex[localDict[i]] = i;
            }

            std::vector<uint32_t> localOffsets;
            localOffsets.reserve(pageRows);
            for (const std::string& v : page) {
                localOffsets.push_back(localIndex[v]);
            }

            encoded.isLocal     = true;
            encoded.offsetWidth = localOffsetWidth;
            encoded.dictionary  = localDict;
            encoded.offsets     = localOffsets;

            std::pair<std::string, std::string> dm = min_max(localDict);
            encoded.diffMin = dm.first;
            encoded.diffMax = dm.second;

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

            for (const std::string& v : diffDict) {
                cumulativeIndex[v] = static_cast<uint32_t>(cumulativeDictionary.size());
                cumulativeDictionary.push_back(v);
            }

            std::vector<uint32_t> diffOffsets;
            diffOffsets.reserve(pageRows);
            for (const std::string& v : page) {
                diffOffsets.push_back(cumulativeIndex[v]);
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

// =============================================================================
// EncoderC1C2 -- Optimizations C1 and C2 combined.
//
// C1: no page copy; range iterators into columnValues.
// C2: localIndex and localOffsets deferred to the if (chooseLocal) branch.
// =============================================================================

EncodedColumn EncoderC1C2::encode(uint32_t colIndex1Based,
                                   const std::vector<std::string>& columnValues) {
    EncodedColumn column;
    column.columnIndex1Based = colIndex1Based;
    column.isString          = is_string_column(columnValues);

    if (!column.isString) {
        column.dictionaryEnabled = false;
        column.rawValues         = columnValues;
        return column;
    }

    column.dictionaryEnabled = true;

    std::vector<std::string>              cumulativeDictionary;
    std::unordered_map<std::string, uint32_t> cumulativeIndex;
    uint8_t  currentOffsetWidth       = 1;
    uint32_t diffCount                = 0;
    uint64_t diffDictionaryBytesTotal = 0;

    const size_t totalRows = columnValues.size();

    for (size_t pageStart = 0; pageStart < totalRows; pageStart += kDefaultPageSize) {

        const size_t pageEnd  = std::min(pageStart + static_cast<size_t>(kDefaultPageSize),
                                         totalRows);
        const size_t pageRows = pageEnd - pageStart;

        // C1: no page copy.
        auto rangeBegin = columnValues.cbegin() + static_cast<long>(pageStart);
        auto rangeEnd   = columnValues.cbegin() + static_cast<long>(pageEnd);

        // Step 1: Cardinality check.
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

        // Step 2: Build localDict only.
        // C2: localIndex and localOffsets deferred.
        std::vector<std::string> localDict(pageSet.begin(), pageSet.end());
        std::sort(localDict.begin(), localDict.end());

        // Step 3: repeatingRows and diffDict.
        // C1: iterate the range, not a copied page vector.
        uint32_t repeatingRows = 0;
        for (auto it = rangeBegin; it != rangeEnd; ++it) {
            if (cumulativeIndex.count(*it) > 0) ++repeatingRows;
        }
        const double repeatRatio = (pageRows == 0)
            ? 0.0
            : static_cast<double>(repeatingRows) / static_cast<double>(pageRows);

        std::vector<std::string> diffDict;
        diffDict.reserve(localDict.size());
        for (const std::string& v : localDict) {
            if (cumulativeIndex.count(v) == 0) diffDict.push_back(v);
        }

        // Step 4: Size estimates.
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

        // Step 5: Decision.
        bool chooseLocal = cumulativeDictionary.empty();

        if (!chooseLocal) {
            if (repeatRatio < kMinCrossPageRepeatRatio) {
                chooseLocal = true;
            } else if (diffOffsetWidth == currentOffsetWidth) {
                chooseLocal = false;
            } else {
                const double diffAvg = (diffCount == 0)
                    ? 0.0
                    : static_cast<double>(diffDictionaryBytesTotal)
                      / static_cast<double>(diffCount);
                const double lhs =
                    static_cast<double>(diffCount)
                        * (static_cast<double>(diffOffsetBytes) - diffAvg)
                    - static_cast<double>(localOffsetBytes)
                    - (static_cast<double>(localPageSize) - static_cast<double>(diffPageSize));
                chooseLocal = (lhs > 0.0);
            }
        }

        // Step 6: Encode the page.
        EncodedPage encoded;
        encoded.rowCount = static_cast<uint32_t>(pageRows);

        // C1: pageMin/pageMax via minmax_element on the range.
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
