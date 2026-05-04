#pragma once
#include "ac_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ac {

// Page-level skip statistics collected during a filtered scan.
// Mirrors the breakdown in the paper (Tables 2 and 3, section 4.3).
struct ScanStats {
    uint32_t totalPages            = 0;
    uint32_t pagesSkippedZoneMap   = 0;  // target outside [pageMin, pageMax]
    uint32_t pagesSkippedDictMiss  = 0;  // target absent from cumulative dict
    uint32_t pagesScanned          = 0;  // pages where offsets were examined
    uint32_t rowsMatched           = 0;
};

// Scan col for all rows where the value equals target.
// Returns 0-based row indices in ascending order.
std::vector<uint32_t> filtered_scan(const EncodedColumn& col,
                                     const std::string& target);

// Same scan, additionally writing skip statistics into stats.
std::vector<uint32_t> filtered_scan(const EncodedColumn& col,
                                     const std::string& target,
                                     ScanStats& stats);

} // namespace ac
