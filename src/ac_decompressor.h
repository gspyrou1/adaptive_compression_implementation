#pragma once

#include "ac_types.h"

// Full decompression (materialisation) of a single encoded column.
//
// This is the inverse of AdaptiveDictionaryEncoder::encode().
// See paper section 3.2.2: "Full Scan / Decompression".

namespace ac {

// Reconstructs all original string values from an encoded column, in row order.
DecodedColumn decompress_column(const EncodedColumn& col);

} // namespace ac
