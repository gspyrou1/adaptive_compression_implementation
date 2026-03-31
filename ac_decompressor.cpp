#include "ac_decompressor.h"

#include <stdexcept>

namespace ac {

// Full decompression algorithm (paper section 3.2.2).
//
// The encoder organised pages into differential sequences:
//
//   [local page] [diff page] [diff page] ... [local page] [diff page] ...
//
// To reverse this, we maintain a cumulative dictionary that we update
// as we walk through the pages in order:
//
//   local page -- RESET the cumulative dictionary to this page's dictionary.
//   diff  page -- APPEND this page's (new-values-only) dictionary to the
//                 cumulative dictionary.
//
// In both cases, every row offset then indexes directly into the cumulative
// dictionary to retrieve the original string value.
DecodedColumn decompress_column(const EncodedColumn& col) {
    DecodedColumn result;
    result.columnIndex1Based = col.columnIndex1Based;

    // Non-string columns were stored as raw values; nothing to decode.
    if (!col.dictionaryEnabled) {
        result.values = col.rawValues;
        return result;
    }

    // Pre-allocate the output: total rows = sum of rowCount across all pages.
    size_t totalRows = 0;
    for (const EncodedPage& page : col.pages) {
        totalRows += page.rowCount;
    }
    result.values.reserve(totalRows);

    std::vector<std::string> cumulativeDict;

    for (const EncodedPage& page : col.pages) {

        if (page.isLocal) {
            // Local page: replace the cumulative dictionary entirely.
            // This page is self-contained, just like an I-frame in video.
            cumulativeDict = page.dictionary;
        } else {
            // Differential page: only new values are stored.
            // Append them to rebuild the cumulative dictionary up to this point.
            for (const std::string& v : page.dictionary) {
                cumulativeDict.push_back(v);
            }
        }

        // Decode every row: each offset is an index into cumulativeDict.
        for (uint32_t offset : page.offsets) {
            if (offset >= cumulativeDict.size()) {
                throw std::runtime_error("Corrupt data: offset out of range");
            }
            result.values.push_back(cumulativeDict[offset]);
        }
    }

    return result;
}

} // namespace ac
