#pragma once

#include <cstdint>
#include <string>
#include <vector>

// All types and constants shared across the adaptive compression implementation.
// Other files include this header as their first dependency.

namespace ac {

// Maximum rows per page. Keeps offset indices within 2 bytes (2^16 = 65536).
constexpr uint32_t kDefaultPageSize = 1u << 16;

// If more than this fraction of a page's values are distinct,
// fall back to local encoding for that page.
constexpr double kDistinctRatioDisable = 0.80;

// If fewer than this fraction of a page's rows contain values seen in previous
// pages of the same sequence, differential encoding offers little benefit.
constexpr double kMinCrossPageRepeatRatio = 0.10;

// One compressed page of a column.
//
// A page is one of three types:
//   local       -- self-contained; dictionary holds all distinct values in this page.
//   differential -- only NEW values (not seen in earlier pages of the sequence) are
//                   stored in the dictionary; offsets index into the cumulative dictionary
//                   built from the local page + all prior differential pages.
//   raw         -- distinct-ratio exceeded 80%; values are stored verbatim with no
//                  dictionary or offsets. rawValues holds one entry per row.
//                  Raw pages break the current differential sequence.
struct EncodedPage {
    bool     isLocal      = true;
    bool     isRaw        = false;  // true when distinct ratio > kDistinctRatioDisable
    uint8_t  offsetWidth  = 1;     // bytes per offset: 1, 2, or 4
    uint32_t rowCount     = 0;

    std::vector<std::string> dictionary;  // local: all distinct values; diff: new values only
    std::vector<uint32_t>    offsets;     // one index per row into the (cumulative) dictionary
    std::vector<std::string> rawValues;   // raw pages only: per-row values (no dictionary)

    std::string pageMin;   // min value in this page (used as a zone map)
    std::string pageMax;   // max value in this page (used as a zone map)
    std::string diffMin;   // min of newly added values (tighter filter for differential pages)
    std::string diffMax;   // max of newly added values

    // Distance (in pages) back to the local page that started this sequence.
    // Always 0 for local pages. For differential pages: 1 for the first diff
    // after a local, 2 for the second, etc.
    uint32_t diffDepth = 0;

    // Sizes of every dictionary in the sequence, from the local page to this
    // page inclusive.  Empty for local pages.  Stored in the page header so
    // that random_access can binary-search for the page that owns a given
    // offset without rebuilding the cumulative dictionary: a prefix-sum scan
    // over dictSizes identifies the right page in O(sequence_length), then
    // only that one dictionary page needs to be read -- two page reads total.
    std::vector<uint32_t> dictSizes;
};

// All compressed pages for one column, plus bookkeeping metadata.
struct EncodedColumn {
    uint32_t columnIndex1Based  = 0;
    bool     isString           = false;
    bool     dictionaryEnabled  = false;

    std::vector<std::string>  rawValues;  // used when dictionaryEnabled == false
    std::vector<EncodedPage>  pages;
};

// Result of decompressing one column: the original string values in row order.
struct DecodedColumn {
    uint32_t                 columnIndex1Based = 0;
    std::vector<std::string> values;
};

} // namespace ac
