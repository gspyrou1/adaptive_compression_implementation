#pragma once

#include <string>
#include <vector>

// CSV reading utilities.
// Keeping I/O separate from encoding makes it easy to swap in
// a different input format (e.g. Parquet, TSV) later.

namespace ac {

// Parses one CSV line into fields.
// Handles double-quoted fields and escaped quotes ("").
std::vector<std::string> parse_csv_line(const std::string& line);

// Reads a CSV file and returns all rows as a 2-D vector.
// Every row is padded or trimmed to match the column count of the first row.
// If max_rows > 0, reading stops after that many rows (useful for very large files).
std::vector<std::vector<std::string>> read_csv(const std::string& path,
                                               size_t max_rows = 0);

} // namespace ac
