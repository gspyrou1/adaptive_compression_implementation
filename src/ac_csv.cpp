#include "ac_csv.h"

#include <fstream>
#include <stdexcept>

namespace ac {

std::vector<std::string> parse_csv_line(const std::string& line) {
    std::vector<std::string> out;
    std::string cell;
    bool inQuotes = false;

    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];

        if (inQuotes) {
            if (c == '"') {
                // Two consecutive quotes inside a quoted field = literal quote character.
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cell.push_back('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                cell.push_back(c);
            }
        } else {
            if (c == ',') {
                out.push_back(cell);
                cell.clear();
            } else if (c == '"') {
                inQuotes = true;
            } else {
                cell.push_back(c);
            }
        }
    }

    out.push_back(cell);  // last field has no trailing comma
    return out;
}

std::vector<std::vector<std::string>> read_csv(const std::string& path,
                                               size_t max_rows) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Failed to open CSV file: " + path);
    }

    std::vector<std::vector<std::string>> rows;
    std::string line;
    size_t expectedCols = 0;

    while (std::getline(input, line)) {
        if (max_rows > 0 && rows.size() >= max_rows) break;

        std::vector<std::string> row = parse_csv_line(line);

        if (expectedCols == 0) {
            expectedCols = row.size();
        }
        // Normalise every row to the same width as the first row.
        if (row.size() != expectedCols) {
            row.resize(expectedCols);
        }

        rows.push_back(row);
    }

    return rows;
}

} // namespace ac
