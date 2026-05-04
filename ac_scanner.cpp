#include "ac_scanner.h"

#include <algorithm>

namespace ac {

std::vector<uint32_t> filtered_scan(const EncodedColumn& col,
                                     const std::string& target) {
    ScanStats stats;
    return filtered_scan(col, target, stats);
}

// Filtered scan (paper section 3.2.3).
//
// Instead of decompressing every row, we exploit three sources of skipping:
//
//  1. Zone maps  (pageMin / pageMax)
//     If target is outside the page's value range it cannot appear in any
//     row, so we skip the offset scan entirely.
//
//  2. Differential min/max  (diffMin / diffMax)
//     If target is outside the range of values newly introduced by a diff
//     page, we know target was not added here -- no need to search the
//     diff dictionary.
//
//  3. Dict miss
//     If target is not in the cumulative dictionary after all of the above,
//     the page contains no matching rows and we skip the offset scan.
//
// Unlike full decompression, we never rebuild the cumulative dictionary.
// We only track whether target is in it and at which index.
std::vector<uint32_t> filtered_scan(const EncodedColumn& col,
                                     const std::string& target,
                                     ScanStats& stats) {
    stats = ScanStats();
    std::vector<uint32_t> result;

    if (!col.dictionaryEnabled) {
        // Non-string column stored as raw values: linear scan.
        for (uint32_t i = 0; i < static_cast<uint32_t>(col.rawValues.size()); ++i) {
            if (col.rawValues[i] == target) result.push_back(i);
        }
        stats.rowsMatched = static_cast<uint32_t>(result.size());
        return result;
    }

    uint32_t rowStart       = 0;
    int64_t  targetIdx      = -1;  // index of target in cumulative dict; -1 = absent
    uint32_t cumulativeSize = 0;   // number of entries in the virtual cumulative dict

    for (const EncodedPage& page : col.pages) {
        ++stats.totalPages;
        const uint32_t rows = page.rowCount;

        if (page.isLocal) {
            // ------------------------------------------------------------------
            // Local page: the cumulative dictionary resets to this page's
            // (sorted) dictionary.  Re-evaluate whether target is present.
            // ------------------------------------------------------------------
            cumulativeSize = static_cast<uint32_t>(page.dictionary.size());
            targetIdx      = -1;

            // Zone map: pageMin/pageMax == min/max of the local dictionary,
            // so a miss here guarantees target is not in the dictionary either.
            if (target < page.pageMin || target > page.pageMax) {
                ++stats.pagesSkippedZoneMap;
                rowStart += rows;
                continue;
            }

            // Binary search in the sorted local dictionary.
            auto it = std::lower_bound(page.dictionary.begin(),
                                       page.dictionary.end(), target);
            if (it != page.dictionary.end() && *it == target) {
                targetIdx = static_cast<int64_t>(
                    std::distance(page.dictionary.begin(), it));
            }

            if (targetIdx == -1) {
                ++stats.pagesSkippedDictMiss;
                rowStart += rows;
                continue;
            }

        } else {
            // ------------------------------------------------------------------
            // Differential page: cumulative dictionary grows by this page's
            // new values.  We only need to check whether target appears among
            // those new values; if it already has a known index from a prior
            // page, that index stays valid.
            // ------------------------------------------------------------------

            // diffMin/diffMax covers the new values only.  If target is
            // outside that range, it was not introduced by this diff page.
            if (!(target < page.diffMin || target > page.diffMax)) {
                auto it = std::lower_bound(page.dictionary.begin(),
                                           page.dictionary.end(), target);
                if (it != page.dictionary.end() && *it == target) {
                    // Target found as a newly introduced value.
                    targetIdx = static_cast<int64_t>(cumulativeSize) +
                                static_cast<int64_t>(
                                    std::distance(page.dictionary.begin(), it));
                }
            }

            // Always advance cumulative size, even when we skip the offset scan
            // below -- subsequent pages depend on the correct cumulative count.
            cumulativeSize += static_cast<uint32_t>(page.dictionary.size());

            // Zone map: target does not appear in any row of this page.
            // We still updated cumulative state above so future pages are correct.
            if (target < page.pageMin || target > page.pageMax) {
                ++stats.pagesSkippedZoneMap;
                rowStart += rows;
                continue;
            }

            if (targetIdx == -1) {
                ++stats.pagesSkippedDictMiss;
                rowStart += rows;
                continue;
            }
        }

        // ------------------------------------------------------------------
        // Offset scan: target is in the cumulative dictionary at targetIdx.
        // Scan this page's offsets and collect matching row numbers.
        // ------------------------------------------------------------------
        ++stats.pagesScanned;
        const uint32_t tidx = static_cast<uint32_t>(targetIdx);
        for (uint32_t i = 0; i < rows; ++i) {
            if (page.offsets[i] == tidx) {
                result.push_back(rowStart + i);
            }
        }
        rowStart += rows;
    }

    stats.rowsMatched = static_cast<uint32_t>(result.size());
    return result;
}

} // namespace ac
