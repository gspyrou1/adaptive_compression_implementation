#include "ac_decompressor.h"
#include "ac_serial.h"
#include "ac_types.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            std::cerr << "Usage: " << argv[0] << " <file.comp>\n";
            return 1;
        }

        std::ifstream in(argv[1], std::ios::binary);
        if (!in) {
            throw std::runtime_error("Failed to open file: " + std::string(argv[1]));
        }

        // Verify the file magic header.
        char magic[5] = {0};
        in.read(magic, 5);
        if (!in || std::string(magic, 5) != "ACMP1") {
            throw std::runtime_error("Not a valid .comp file (bad magic header)");
        }

        const uint32_t totalRows          = ac::read_u32(in);
        const uint32_t totalColumns       = ac::read_u32(in);  // original column count
        const uint32_t encodedColumnCount = ac::read_u32(in);

        (void)totalColumns;  // stored in the file for reference; not needed for output

        // Read and decompress every encoded column.
        std::vector<ac::DecodedColumn> columns;
        columns.reserve(encodedColumnCount);
        for (uint32_t i = 0; i < encodedColumnCount; ++i) {
            ac::EncodedColumn enc = ac::read_column(in);
            columns.push_back(ac::decompress_column(enc));
        }

        // Sanity check: every column must have exactly totalRows values.
        for (const ac::DecodedColumn& col : columns) {
            if (col.values.size() != totalRows) {
                throw std::runtime_error("Corrupt data: column row count mismatch");
            }
        }

        // Write the decoded columns as CSV to stdout, one row per line.
        for (uint32_t row = 0; row < totalRows; ++row) {
            for (size_t c = 0; c < columns.size(); ++c) {
                if (c > 0) std::cout << ',';
                std::cout << columns[c].values[row];
            }
            std::cout << '\n';
        }

        return 0;

    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return 2;
    }
}
