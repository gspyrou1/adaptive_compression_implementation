#pragma once

#include "ac_types.h"

#include <cstdint>
#include <string>
#include <vector>

// Adaptive dictionary encoder for string columns.
//
// Reference: Foufoulas et al., "Adaptive Compression for Fast Scans on String
// Columns", SIGMOD 2021.  https://doi.org/10.1145/3448016.3452798
//
// Overview
// --------
// The encoder groups pages into differential sequences:
//
//   [local page] [diff page] [diff page] ... [local page] [diff page] ...
//        ^-- sequence start                       ^-- next sequence start
//
// Local page   -- self-contained; stores all distinct values for the page.
//                 Works like a classic block-level dictionary.
//
// Differential -- stores only values NEW to this page (not seen in any prior
//   page        page in the same sequence).  Offsets index into a cumulative
//                dictionary that grows as each differential page is appended.
//
// A cost function (Equation 1 in the paper) decides when to start a fresh
// local page instead of continuing with differential encoding.

namespace ac {

struct AdaptiveDictionaryEncoder {
    // Encodes a single column.
    // colIndex1Based: 1-based column number (stored in the output for reference).
    // columnValues:   all values in the column, in row order.
    EncodedColumn encode(uint32_t colIndex1Based,
                         const std::vector<std::string>& columnValues);
};

} // namespace ac
