#pragma once

#include "ac_types.h"

#include <istream>
#include <ostream>

// Binary serialisation and deserialisation of encoded columns.
// All integers are stored little-endian.

namespace ac {

// ----- Write (compression path) ------------------------------------------

void write_u8    (std::ostream& os, uint8_t v);
void write_u32   (std::ostream& os, uint32_t v);
void write_string(std::ostream& os, const std::string& s);
void write_offset(std::ostream& os, uint32_t value, uint8_t width);
void write_column(std::ostream& os, const EncodedColumn& col);

// ----- Read (decompression path) -----------------------------------------

uint8_t      read_u8    (std::istream& is);
uint32_t     read_u32   (std::istream& is);
std::string  read_string(std::istream& is);
uint32_t     read_offset(std::istream& is, uint8_t width);
EncodedColumn read_column(std::istream& is);

} // namespace ac
