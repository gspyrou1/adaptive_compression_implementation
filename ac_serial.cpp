#include "ac_serial.h"

#include <stdexcept>

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

// ----- Read (decompression path) -----------------------------------------

uint8_t read_u8(std::istream& is) {
    unsigned char c = 0;
    is.read(reinterpret_cast<char*>(&c), 1);
    if (!is) throw std::runtime_error("Unexpected end of compressed file");
    return static_cast<uint8_t>(c);
}

uint32_t read_u32(std::istream& is) {
    // Little-endian: least significant byte first.
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(read_u8(is)) << (i * 8);
    }
    return v;
}

std::string read_string(std::istream& is) {
    const uint32_t len = read_u32(is);
    std::string s(static_cast<size_t>(len), '\0');
    if (len > 0) {
        is.read(&s[0], static_cast<std::streamsize>(len));
        if (!is) throw std::runtime_error("Unexpected end of compressed file");
    }
    return s;
}

uint32_t read_offset(std::istream& is, uint8_t width) {
    // Same little-endian layout as write_offset.
    uint32_t v = 0;
    for (uint8_t i = 0; i < width; ++i) {
        v |= static_cast<uint32_t>(read_u8(is)) << (i * 8);
    }
    return v;
}

EncodedColumn read_column(std::istream& is) {
    EncodedColumn col;
    col.columnIndex1Based = read_u32(is);
    col.isString          = (read_u8(is) == 1);
    col.dictionaryEnabled = (read_u8(is) == 1);

    if (!col.dictionaryEnabled) {
        const uint32_t count = read_u32(is);
        col.rawValues.resize(static_cast<size_t>(count));
        for (uint32_t i = 0; i < count; ++i) {
            col.rawValues[i] = read_string(is);
        }
        return col;
    }

    // kDefaultPageSize is stored for reference but not needed during decoding.
    read_u32(is);

    const uint32_t pageCount = read_u32(is);
    col.pages.resize(static_cast<size_t>(pageCount));

    for (uint32_t p = 0; p < pageCount; ++p) {
        EncodedPage& page = col.pages[p];

        // isLocal: 0 = local, 1 = differential (mirrors write_column).
        page.isLocal     = (read_u8(is) == 0);
        page.offsetWidth = read_u8(is);
        page.rowCount    = read_u32(is);

        page.pageMin = read_string(is);
        page.pageMax = read_string(is);
        page.diffMin = read_string(is);
        page.diffMax = read_string(is);

        const uint32_t dictSize = read_u32(is);
        page.dictionary.resize(static_cast<size_t>(dictSize));
        for (uint32_t i = 0; i < dictSize; ++i) {
            page.dictionary[i] = read_string(is);
        }

        const uint32_t offsetCount = read_u32(is);
        page.offsets.resize(static_cast<size_t>(offsetCount));
        for (uint32_t i = 0; i < offsetCount; ++i) {
            page.offsets[i] = read_offset(is, page.offsetWidth);
        }
    }

    return col;
}

} // namespace ac
