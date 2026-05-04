#include "ac_random_access.h"

#include <stdexcept>

namespace ac {

// Random access / point query (paper section 3.2.5).
//
// For a local page the cumulative dictionary is replaced by that page's
// dictionary, exactly as in full decompression.  For a differential page
// the new values are appended.  We stop walking as soon as we locate the
// page that contains rowIndex, perform a single offset lookup, and return.
//
// Cost: O(pages_before_target * avg_dict_size) for cumulative dict
// construction plus O(1) for the final lookup.  The paper notes that a
// further optimisation is to walk backwards from the target page to its
// sequence-start (nearest prior local page) and rebuild only that shorter
// prefix, avoiding pages before the sequence entirely.  That trade-off is
// most valuable when pages are stored on disk; for in-memory columns this
// forward walk is simple and sufficient.
std::string random_access(const EncodedColumn& col, uint32_t rowIndex) {
    if (!col.dictionaryEnabled) {
        if (rowIndex >= col.rawValues.size()) {
            throw std::out_of_range("Row index out of range");
        }
        return col.rawValues[rowIndex];
    }

    uint32_t                rowStart = 0;
    std::vector<std::string> cumulativeDict;

    for (const EncodedPage& page : col.pages) {

        // Maintain cumulative dictionary regardless of whether this is the
        // target page -- we need it to be correct when we arrive.
        if (page.isLocal) {
            cumulativeDict = page.dictionary;
        } else {
            for (const std::string& v : page.dictionary) {
                cumulativeDict.push_back(v);
            }
        }

        const uint32_t pageEnd = rowStart + page.rowCount;

        if (rowIndex < pageEnd) {
            // Target row is in this page.
            const uint32_t withinPage = rowIndex - rowStart;
            const uint32_t offset     = page.offsets[withinPage];

            if (offset >= cumulativeDict.size()) {
                throw std::runtime_error("Corrupt data: offset out of range");
            }
            return cumulativeDict[offset];
        }

        rowStart = pageEnd;
    }

    throw std::out_of_range("Row index out of range");
}

} // namespace ac
