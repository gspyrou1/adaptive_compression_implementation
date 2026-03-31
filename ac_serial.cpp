#include "ac_serial.h"

namespace ac {

void write_u8(std::ostream& os, uint8_t v) {
    os.put(static_cast<char>(v));
}

void write_u32(std::ostream& os, uint32_t v) {
    // Little-endian: least significant byte first.
    for (int i = 0; i < 4; ++i) {
        write_u8(os, static_cast<uint8_t>((v >> (i * 8)) & 0xFFu));
    }
}

void write_string(std::ostream& os, const std::string& s) {
    write_u32(os, static_cast<uint32_t>(s.size()));
    os.write(s.data(), static_cast<std::streamsize>(s.size()));
}

void write_offset(std::ostream& os, uint32_t value, uint8_t width) {
    // Same little-endian layout as write_u32, but only 'width' bytes are written.
    for (uint8_t i = 0; i < width; ++i) {
        write_u8(os, static_cast<uint8_t>((value >> (i * 8)) & 0xFFu));
    }
}

void write_column(std::ostream& os, const EncodedColumn& col) {
    write_u32(os, col.columnIndex1Based);
    write_u8 (os, col.isString          ? 1 : 0);
    write_u8 (os, col.dictionaryEnabled ? 1 : 0);

    if (!col.dictionaryEnabled) {
        // Non-string columns: write raw values.
        write_u32(os, static_cast<uint32_t>(col.rawValues.size()));
        for (const std::string& v : col.rawValues) {
            write_string(os, v);
        }
        return;
    }

    write_u32(os, kDefaultPageSize);
    write_u32(os, static_cast<uint32_t>(col.pages.size()));

    for (const EncodedPage& page : col.pages) {
        write_u8 (os, page.isLocal ? 0 : 1);
        write_u8 (os, page.offsetWidth);
        write_u32(os, page.rowCount);

        write_string(os, page.pageMin);
        write_string(os, page.pageMax);
        write_string(os, page.diffMin);
        write_string(os, page.diffMax);

        write_u32(os, static_cast<uint32_t>(page.dictionary.size()));
        for (const std::string& value : page.dictionary) {
            write_string(os, value);
        }

        write_u32(os, static_cast<uint32_t>(page.offsets.size()));
        for (uint32_t offset : page.offsets) {
            write_offset(os, offset, page.offsetWidth);
        }
    }
}

} // namespace ac
