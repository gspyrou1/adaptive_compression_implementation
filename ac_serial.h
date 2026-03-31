#pragma once

#include "ac_types.h"

#include <ostream>

// Binary serialisation of encoded columns.
//
// The write_* functions emit little-endian binary data to any std::ostream.
// A matching set of read_* functions (for decompression / scanning) will
// be added in ac_serial.cpp when that phase of the paper is implemented.

namespace ac {

void write_u8    (std::ostream& os, uint8_t v);
void write_u32   (std::ostream& os, uint32_t v);
void write_string(std::ostream& os, const std::string& s);
void write_offset(std::ostream& os, uint32_t value, uint8_t width);
void write_column(std::ostream& os, const EncodedColumn& col);

} // namespace ac
