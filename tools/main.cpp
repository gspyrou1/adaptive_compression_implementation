#include "ac_csv.h"
#include "ac_encoder.h"
#include "ac_serial.h"
#include "ac_types.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

// Parse the column indices given on the command line (1-based, deduplicated, sorted).
static std::vector<uint32_t> parse_selected_columns(int argc, char** argv) {
    std::vector<uint32_t> cols;
    for (int i = 2; i < argc; ++i) {
        const long idx = std::strtol(argv[i], nullptr, 10);
        if (idx <= 0 || idx > static_cast<long>(std::numeric_limits<uint32_t>::max())) {
            throw std::runtime_error("Invalid column index: " + std::string(argv[i]));
        }
        cols.push_back(static_cast<uint32_t>(idx));
    }
    std::sort(cols.begin(), cols.end());
    cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
    return cols;
}

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            std::cerr << "Usage: " << argv[0] << " <csv-file> <col1> [col2 ...] > file.comp\n";
            return 1;
        }

        const std::string csvPath = argv[1];
        const std::vector<uint32_t> selectedColumns = parse_selected_columns(argc, argv);

        const std::vector<std::vector<std::string>> rows = ac::read_csv(csvPath);
        if (rows.empty()) {
            throw std::runtime_error("CSV is empty.");
        }
        const size_t colCount = rows.front().size();

        // Encode each requested column.
        std::vector<ac::EncodedColumn> encodedColumns;
        encodedColumns.reserve(selectedColumns.size());

        ac::AdaptiveDictionaryEncoder encoder;
        for (uint32_t colIndex1Based : selectedColumns) {
            if (colIndex1Based > colCount) {
                throw std::runtime_error("Column index out of range: "
                                         + std::to_string(colIndex1Based));
            }

            // Extract the column values from the 2-D row table.
            std::vector<std::string> values;
            values.reserve(rows.size());
            const size_t col0 = static_cast<size_t>(colIndex1Based - 1);
            for (const std::vector<std::string>& row : rows) {
                values.push_back(row[col0]);
            }

            encodedColumns.push_back(encoder.encode(colIndex1Based, values));
        }

        // Write file header, then each compressed column, to stdout.
        std::ostream& out = std::cout;
        out.write("ACMP1", 5);
        ac::write_u32(out, static_cast<uint32_t>(rows.size()));
        ac::write_u32(out, static_cast<uint32_t>(colCount));
        ac::write_u32(out, static_cast<uint32_t>(encodedColumns.size()));

        for (const ac::EncodedColumn& col : encodedColumns) {
            ac::write_column(out, col);
        }

        out.flush();
        return 0;

    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return 2;
    }
}
