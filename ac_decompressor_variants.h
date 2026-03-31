#pragma once

#include "ac_types.h"

// Decompressor variants for benchmarking individual optimizations.
//
//   decompress_column_d1 -- D1 only: pointer-based cumulative dictionary.

namespace ac {

// D1: replace the std::vector<std::string> cumulative dictionary with
// std::vector<const std::string*>. On both local and differential pages,
// the cumulative dictionary is populated with pointers into the already-
// allocated page.dictionary vectors rather than copying the strings.
// The output result.values still gets full string copies (unavoidable for
// a materialized result), so the saving is in the intermediate working
// structure only.
DecodedColumn decompress_column_d1(const EncodedColumn& col);

} // namespace ac
