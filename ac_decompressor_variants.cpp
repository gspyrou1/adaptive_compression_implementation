#include "ac_decompressor_variants.h"

#include <stdexcept>

namespace ac {

// =============================================================================
// decompress_column_d1 -- Optimization D1 only: pointer-based cumulative dict.
//
// Baseline copies all strings from page.dictionary into cumulativeDict on
// every local page (k full string copies per sequence start), and again on
// every differential page (j copies per new-value batch).  Those copies exist
// only to support the offset->value lookup; they add no information.
//
// Change: cumulativeDict becomes a vector of const std::string* instead of
// std::vector<std::string>. Entries point directly into the page.dictionary
// vectors of the EncodedColumn, which remain valid for the entire call.
// Populating cumulativeDict is now k/j pointer assignments instead of string
// copies.  The only string copies that remain are the ones into result.values,
// which are required to materialise the output and cannot be eliminated here.
//
// Everything else (logic, bounds check, sequence handling) is identical to
// the baseline decompress_column.
// =============================================================================

DecodedColumn decompress_column_d1(const EncodedColumn& col) {
    DecodedColumn result;
    result.columnIndex1Based = col.columnIndex1Based;

    if (!col.dictionaryEnabled) {
        result.values = col.rawValues;
        return result;
    }

    size_t totalRows = 0;
    for (const EncodedPage& page : col.pages) {
        totalRows += page.rowCount;
    }
    result.values.reserve(totalRows);

    // D1: pointer vector instead of string vector.
    // Pointers stay valid because col (and its pages) outlives this function.
    std::vector<const std::string*> cumulativeDict;

    for (const EncodedPage& page : col.pages) {

        if (page.isLocal) {
            // D1: reset by storing pointers into page.dictionary, no string copies.
            cumulativeDict.clear();
            for (const std::string& v : page.dictionary) {
                cumulativeDict.push_back(&v);
            }
        } else {
            // D1: append by storing pointers into page.dictionary, no string copies.
            for (const std::string& v : page.dictionary) {
                cumulativeDict.push_back(&v);
            }
        }

        for (uint32_t offset : page.offsets) {
            if (offset >= cumulativeDict.size()) {
                throw std::runtime_error("Corrupt data: offset out of range");
            }
            // Dereference the pointer to get the string for the output copy.
            result.values.push_back(*cumulativeDict[offset]);
        }
    }

    return result;
}

} // namespace ac
