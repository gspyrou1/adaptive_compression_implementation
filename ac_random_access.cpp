#include "ac_random_access.h"

#include <stdexcept>

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

    // Step 2b: differential page -- jump to sequence start using diffDepth,
    // then rebuild the cumulative dictionary only for this sequence prefix.
    const uint32_t localPageIdx = pageIdx - targetPage.diffDepth;

    std::vector<std::string> cumulativeDict;
    for (uint32_t i = localPageIdx; i <= pageIdx; ++i) {
        const EncodedPage& p = col.pages[i];
        if (p.isLocal) {
            cumulativeDict = p.dictionary;
        } else {
            for (const std::string& v : p.dictionary) {
                cumulativeDict.push_back(v);
            }
        }
    }

    if (offset >= static_cast<uint32_t>(cumulativeDict.size())) {
        throw std::runtime_error("Corrupt data: offset out of range");
    }
    return cumulativeDict[offset];
}

} // namespace ac
