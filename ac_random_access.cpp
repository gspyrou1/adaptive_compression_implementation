#include "ac_random_access.h"

#include <stdexcept>
#include <vector>

namespace ac {

// Random access / point query (paper section 3.2.5).
//
// Strategy:
//   1. Walk pages forward to locate the page containing rowIndex and record
//      its index in the pages vector.  This is O(total_pages) but touches
//      only the lightweight page headers -- no dictionaries or offsets are
//      read until we know the exact target page.
//
//   2a. If the target page is local, its dictionary is self-contained.
//       One direct lookup into page.dictionary finishes the query.
//
//   2b. If the target page is differential, diffDepth tells us exactly how
//       many pages back the sequence-start local page is.  We jump directly
//       to it and rebuild the cumulative dictionary only for the pages
//       [localPageIdx .. targetPageIdx] -- O(sequence_length), not
//       O(total_pages).  For typical sequences (tens of pages) this is far
//       cheaper than rebuilding from the beginning of the column.
std::string random_access(const EncodedColumn& col, uint32_t rowIndex) {
    if (!col.dictionaryEnabled) {
        if (rowIndex >= static_cast<uint32_t>(col.rawValues.size())) {
            throw std::out_of_range("Row index out of range");
        }
        return col.rawValues[rowIndex];
    }

    // Step 1: find which page contains rowIndex.
    uint32_t rowStart = 0;
    uint32_t pageIdx  = 0;
    for (; pageIdx < static_cast<uint32_t>(col.pages.size()); ++pageIdx) {
        const uint32_t pageEnd = rowStart + col.pages[pageIdx].rowCount;
        if (rowIndex < pageEnd) break;
        rowStart = pageEnd;
    }
    if (pageIdx >= static_cast<uint32_t>(col.pages.size())) {
        throw std::out_of_range("Row index out of range");
    }

    const EncodedPage& targetPage  = col.pages[pageIdx];
    const uint32_t     withinPage  = rowIndex - rowStart;
    const uint32_t     offset      = targetPage.offsets[withinPage];

    // Step 2a: local page -- dictionary is self-contained.
    if (targetPage.isLocal) {
        if (offset >= static_cast<uint32_t>(targetPage.dictionary.size())) {
            throw std::runtime_error("Corrupt data: offset out of range");
        }
        return targetPage.dictionary[offset];
    }

    // Step 2b: differential page -- two-page read (paper §3.2.5).
    //
    // dictSizes holds the size of every dictionary in the sequence, from the
    // local page to this page inclusive.  A prefix-sum scan over dictSizes
    // identifies which page in the sequence owns the offset, so we can read
    // that one page's dictionary directly without rebuilding the cumulative
    // structure.
    const uint32_t localPageIdx = pageIdx - targetPage.diffDepth;
    const std::vector<uint32_t>& sizes = targetPage.dictSizes;

    if (sizes.empty()) {
        throw std::runtime_error("Corrupt data: dictSizes missing on differential page");
    }

    uint32_t cumSum = 0;
    uint32_t seqPos = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(sizes.size()); ++i) {
        if (offset < cumSum + sizes[i]) {
            seqPos = i;
            break;
        }
        cumSum += sizes[i];
    }

    const EncodedPage& dictPage   = col.pages[localPageIdx + seqPos];
    const uint32_t     dictOffset = offset - cumSum;

    if (dictOffset >= static_cast<uint32_t>(dictPage.dictionary.size())) {
        throw std::runtime_error("Corrupt data: offset out of range");
    }
    return dictPage.dictionary[dictOffset];
}

// Batch random access (§3.2.4).
//
// Strategy:
//   Walk the column's pages sequentially, maintaining the cumulative dictionary
//   incrementally (same as full decompression).  For each page, binary-search
//   the sorted rowIndices to find the subset that falls within that page's row
//   range, then resolve each one from the already-built cumulative dictionary.
//
//   Cost: O(total_pages + total_indices).  The cumulative dictionary is rebuilt
//   at most once per page regardless of how many indices land in that page --
//   far cheaper than calling random_access() once per index (each of which
//   would independently rebuild it up to that page).
std::vector<std::string> batch_random_access(const EncodedColumn& col,
                                              const std::vector<uint32_t>& rowIndices) {
    if (rowIndices.empty()) return {};

    if (!col.dictionaryEnabled) {
        std::vector<std::string> result;
        result.reserve(rowIndices.size());
        for (uint32_t idx : rowIndices) {
            if (idx >= static_cast<uint32_t>(col.rawValues.size())) {
                throw std::out_of_range("Row index out of range");
            }
            result.push_back(col.rawValues[idx]);
        }
        return result;
    }

    std::vector<std::string> result(rowIndices.size());
    std::vector<std::string> cumulativeDict;

    uint32_t rowStart = 0;
    size_t   queryPos = 0;  // next unprocessed position in rowIndices

    for (const EncodedPage& page : col.pages) {
        const uint32_t rowEnd = rowStart + page.rowCount;

        // Find all query indices that land in [rowStart, rowEnd).
        // rowIndices is sorted so we can scan forward from queryPos.
        const size_t pageQueryStart = queryPos;
        while (queryPos < rowIndices.size() && rowIndices[queryPos] < rowEnd) {
            ++queryPos;
        }
        const size_t pageQueryEnd = queryPos;

        // Maintain the cumulative dictionary for all pages we walk, even those
        // with no matching indices -- subsequent diff pages depend on it.
        if (page.isLocal) {
            cumulativeDict = page.dictionary;
        } else {
            cumulativeDict.insert(cumulativeDict.end(),
                                  page.dictionary.begin(),
                                  page.dictionary.end());
        }

        // Resolve each query index that falls in this page.
        for (size_t qi = pageQueryStart; qi < pageQueryEnd; ++qi) {
            const uint32_t withinPage = rowIndices[qi] - rowStart;
            const uint32_t offset     = page.offsets[withinPage];
            if (offset >= static_cast<uint32_t>(cumulativeDict.size())) {
                throw std::runtime_error("Corrupt data: offset out of range");
            }
            result[qi] = cumulativeDict[offset];
        }

        rowStart = rowEnd;
        if (queryPos >= rowIndices.size()) break;  // all queries answered
    }

    return result;
}

} // namespace ac
