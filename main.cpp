#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kDefaultPageSize = 1u << 16;
constexpr double kDistinctRatioDisable = 0.80;
constexpr double kMinCrossPageRepeatRatio = 0.10;

struct EncodedPage {
	bool isLocal = true;
	uint8_t offsetWidth = 1;
	uint32_t rowCount = 0;
	std::vector<std::string> dictionary;
	std::vector<uint32_t> offsets;
	std::string pageMin;
	std::string pageMax;
	std::string diffMin;
	std::string diffMax;
};

struct EncodedColumn {
	uint32_t columnIndex1Based = 0;
	bool isString = false;
	bool dictionaryEnabled = false;
	std::vector<std::string> rawValues;
	std::vector<EncodedPage> pages;
};

[[nodiscard]] uint8_t width_for_cardinality(uint32_t cardinality) {
	if (cardinality <= 0x100u) {
		return 1;
	}
	if (cardinality <= 0x10000u) {
		return 2;
	}
	return 4;
}

[[nodiscard]] bool is_numeric(const std::string& s) {
	if (s.empty()) {
		return false;
	}
	char* end = nullptr;
	errno = 0;
	std::strtod(s.c_str(), &end);
	if (errno != 0) {
		return false;
	}
	return end != nullptr && *end == '\0';
}

[[nodiscard]] std::vector<std::string> parse_csv_line(const std::string& line) {
	std::vector<std::string> out;
	std::string cell;
	bool inQuotes = false;

	for (size_t i = 0; i < line.size(); ++i) {
		const char c = line[i];
		if (inQuotes) {
			if (c == '"') {
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
	out.push_back(cell);
	return out;
}

[[nodiscard]] std::vector<std::vector<std::string>> read_csv(const std::string& path) {
	std::ifstream input(path);
	if (!input) {
		throw std::runtime_error("Failed to open CSV file: " + path);
	}

	std::vector<std::vector<std::string>> rows;
	std::string line;
	size_t expectedCols = 0;
	while (std::getline(input, line)) {
		auto row = parse_csv_line(line);
		if (expectedCols == 0) {
			expectedCols = row.size();
		}
		if (row.size() < expectedCols) {
			row.resize(expectedCols);
		} else if (row.size() > expectedCols) {
			row.resize(expectedCols);
		}
		rows.push_back(std::move(row));
	}
	return rows;
}

[[nodiscard]] bool is_string_column(const std::vector<std::string>& values) {
	for (const auto& v : values) {
		if (!is_numeric(v)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] uint64_t dictionary_storage_bytes(const std::vector<std::string>& values) {
	uint64_t bytes = 0;
	for (const auto& v : values) {
		bytes += sizeof(uint32_t);
		bytes += static_cast<uint64_t>(v.size());
	}
	return bytes;
}

[[nodiscard]] std::pair<std::string, std::string> min_max(const std::vector<std::string>& vals) {
	if (vals.empty()) {
		return {"", ""};
	}
	auto [itMin, itMax] = std::minmax_element(vals.begin(), vals.end());
	return {*itMin, *itMax};
}

struct AdaptiveDictionaryEncoder {
	EncodedColumn encode(uint32_t colIndex1Based, const std::vector<std::string>& columnValues) {
		EncodedColumn column;
		column.columnIndex1Based = colIndex1Based;
		column.isString = is_string_column(columnValues);
		if (!column.isString) {
			column.dictionaryEnabled = false;
			column.rawValues = columnValues;
			return column;
		}

		column.dictionaryEnabled = true;

		std::vector<std::string> cumulativeDictionary;
		std::unordered_map<std::string, uint32_t> cumulativeIndex;
		uint8_t currentOffsetWidth = 1;
		uint32_t diffCount = 0;
		uint64_t diffDictionaryBytesTotal = 0;

		for (size_t pageStart = 0; pageStart < columnValues.size(); pageStart += kDefaultPageSize) {
			const size_t pageEnd = std::min(pageStart + static_cast<size_t>(kDefaultPageSize), columnValues.size());
			const size_t pageRows = pageEnd - pageStart;
			std::vector<std::string> page(columnValues.begin() + static_cast<long>(pageStart),
										  columnValues.begin() + static_cast<long>(pageEnd));

			std::unordered_set<std::string> pageSet(page.begin(), page.end());
			const double distinctRatio = static_cast<double>(pageSet.size()) / static_cast<double>(pageRows);
			if (distinctRatio > kDistinctRatioDisable) {
				// Fall back to local dictionary pages for this and subsequent pages in highly distinct data.
				cumulativeDictionary.clear();
				cumulativeIndex.clear();
				currentOffsetWidth = 1;
				diffCount = 0;
				diffDictionaryBytesTotal = 0;
			}

			std::vector<std::string> localDict(pageSet.begin(), pageSet.end());
			std::sort(localDict.begin(), localDict.end());
			std::unordered_map<std::string, uint32_t> localIndex;
			localIndex.reserve(localDict.size() * 2 + 1);
			for (uint32_t i = 0; i < localDict.size(); ++i) {
				localIndex[localDict[i]] = i;
			}

			std::vector<uint32_t> localOffsets;
			localOffsets.reserve(pageRows);
			for (const auto& v : page) {
				localOffsets.push_back(localIndex[v]);
			}

			std::vector<std::string> diffDict;
			diffDict.reserve(localDict.size());
			uint32_t repeatingRows = 0;
			for (const auto& v : page) {
				if (cumulativeIndex.find(v) != cumulativeIndex.end()) {
					++repeatingRows;
				}
			}
			const double repeatRatio = pageRows == 0 ? 0.0 : static_cast<double>(repeatingRows) / static_cast<double>(pageRows);

			for (const auto& v : localDict) {
				if (cumulativeIndex.find(v) == cumulativeIndex.end()) {
					diffDict.push_back(v);
				}
			}
			// diffDict is already sorted: built by filtering the sorted localDict in order.

			const uint32_t diffCardinality = static_cast<uint32_t>(cumulativeDictionary.size() + diffDict.size());
			const uint8_t diffOffsetWidth = width_for_cardinality(diffCardinality);
			const uint8_t localOffsetWidth = width_for_cardinality(static_cast<uint32_t>(localDict.size()));

			const uint64_t localDictBytes = dictionary_storage_bytes(localDict);
			const uint64_t diffDictBytes = dictionary_storage_bytes(diffDict);
			const uint64_t localOffsetBytes = static_cast<uint64_t>(pageRows) * localOffsetWidth;
			const uint64_t diffOffsetBytes = static_cast<uint64_t>(pageRows) * diffOffsetWidth;
			const uint64_t localPageSize = localDictBytes + localOffsetBytes;
			const uint64_t diffPageSize = diffDictBytes + diffOffsetBytes;

			// diffcount * (difOffsetSize − diffavg) − locOffsetSize − (locPageSize − difPageSize) > 0 

			bool chooseLocal = cumulativeDictionary.empty();
			if (!chooseLocal) {
				if (repeatRatio < kMinCrossPageRepeatRatio) {
					chooseLocal = true;
				} else if (diffOffsetWidth == currentOffsetWidth) {
					chooseLocal = false;
				} else {
					// diffCount == 0 makes the entire first term zero regardless of diffAvg.
					const double diffAvg = diffCount == 0
											   ? 0.0
											   : static_cast<double>(diffDictionaryBytesTotal) / static_cast<double>(diffCount);
					const double lhs = static_cast<double>(diffCount) *
										   (static_cast<double>(diffOffsetBytes) - diffAvg) -
									   static_cast<double>(localOffsetBytes) -
									   (static_cast<double>(localPageSize) - static_cast<double>(diffPageSize));
					chooseLocal = lhs > 0.0;
				}
			}

			EncodedPage encoded;
			encoded.rowCount = static_cast<uint32_t>(pageRows);
			auto [pMin, pMax] = min_max(page);
			encoded.pageMin = pMin;
			encoded.pageMax = pMax;

			if (chooseLocal) {
				encoded.isLocal = true;
				encoded.offsetWidth = localOffsetWidth;
				encoded.dictionary = localDict;
				encoded.offsets = std::move(localOffsets);

				cumulativeDictionary = localDict;
				cumulativeIndex.clear();
				cumulativeIndex.reserve(cumulativeDictionary.size() * 2 + 1);
				for (uint32_t i = 0; i < cumulativeDictionary.size(); ++i) {
					cumulativeIndex[cumulativeDictionary[i]] = i;
				}
				currentOffsetWidth = width_for_cardinality(static_cast<uint32_t>(cumulativeDictionary.size()));
				diffCount = 0;
				diffDictionaryBytesTotal = 0;

				auto [dMin, dMax] = min_max(localDict);
				encoded.diffMin = dMin;
				encoded.diffMax = dMax;
			} else {
				encoded.isLocal = false;
				encoded.offsetWidth = diffOffsetWidth;
				encoded.dictionary = diffDict;

				auto [dMin, dMax] = min_max(diffDict);
				encoded.diffMin = dMin;
				encoded.diffMax = dMax;

				std::vector<uint32_t> diffOffsets;
				diffOffsets.reserve(pageRows);

				for (const auto& v : diffDict) {
					cumulativeIndex[v] = static_cast<uint32_t>(cumulativeDictionary.size());
					cumulativeDictionary.push_back(v);
				}
				for (const auto& v : page) {
					diffOffsets.push_back(cumulativeIndex[v]);
				}
				encoded.offsets = std::move(diffOffsets);

				currentOffsetWidth = width_for_cardinality(static_cast<uint32_t>(cumulativeDictionary.size()));
				++diffCount;
				diffDictionaryBytesTotal += diffDictBytes;
			}

			column.pages.push_back(std::move(encoded));
		}

		return column;
	}
};

void write_u8(std::ostream& os, uint8_t v) {
	os.put(static_cast<char>(v));
}

void write_u32(std::ostream& os, uint32_t v) {
	for (int i = 0; i < 4; ++i) {
		write_u8(os, static_cast<uint8_t>((v >> (i * 8)) & 0xFFu));
	}
}

void write_string(std::ostream& os, const std::string& s) {
	write_u32(os, static_cast<uint32_t>(s.size()));
	os.write(s.data(), static_cast<std::streamsize>(s.size()));
}

void write_offset(std::ostream& os, uint32_t value, uint8_t width) {
	for (uint8_t i = 0; i < width; ++i) {
		write_u8(os, static_cast<uint8_t>((value >> (i * 8)) & 0xFFu));
	}
}

void write_column(std::ostream& os, const EncodedColumn& col) {
	write_u32(os, col.columnIndex1Based);
	write_u8(os, col.isString ? 1 : 0);
	write_u8(os, col.dictionaryEnabled ? 1 : 0);

	if (!col.dictionaryEnabled) {
		write_u32(os, static_cast<uint32_t>(col.rawValues.size()));
		for (const auto& v : col.rawValues) {
			write_string(os, v);
		}
		return;
	}

	write_u32(os, kDefaultPageSize);
	write_u32(os, static_cast<uint32_t>(col.pages.size()));
	for (const auto& page : col.pages) {
		write_u8(os, page.isLocal ? 0 : 1);
		write_u8(os, page.offsetWidth);
		write_u32(os, page.rowCount);

		write_string(os, page.pageMin);
		write_string(os, page.pageMax);
		write_string(os, page.diffMin);
		write_string(os, page.diffMax);

		write_u32(os, static_cast<uint32_t>(page.dictionary.size()));
		for (const auto& value : page.dictionary) {
			write_string(os, value);
		}

		write_u32(os, static_cast<uint32_t>(page.offsets.size()));
		for (const auto offset : page.offsets) {
			write_offset(os, offset, page.offsetWidth);
		}
	}
}

[[nodiscard]] std::vector<uint32_t> parse_selected_columns(int argc, char** argv) {
	std::vector<uint32_t> cols;
	cols.reserve(static_cast<size_t>(std::max(0, argc - 2)));
	for (int i = 2; i < argc; ++i) {
		const long idx = std::strtol(argv[i], nullptr, 10);
		if (idx <= 0 || idx > std::numeric_limits<uint32_t>::max()) {
			throw std::runtime_error("Invalid column index: " + std::string(argv[i]));
		}
		cols.push_back(static_cast<uint32_t>(idx));
	}
	std::sort(cols.begin(), cols.end());
	cols.erase(std::unique(cols.begin(), cols.end()), cols.end());
	return cols;
}

}  // namespace

int main(int argc, char** argv) {
	try {
		if (argc < 3) {
			std::cerr << "Usage: " << argv[0] << " <csv-file> <col1> [col2 ...] > file.comp\n";
			return 1;
		}

		const std::string csvPath = argv[1];
		const auto selectedColumns = parse_selected_columns(argc, argv);

		const auto rows = read_csv(csvPath);
		if (rows.empty()) {
			throw std::runtime_error("CSV is empty.");
		}
		const size_t colCount = rows.front().size();

		std::vector<EncodedColumn> encodedColumns;
		encodedColumns.reserve(selectedColumns.size());

		AdaptiveDictionaryEncoder encoder;
		for (const auto colIndex1Based : selectedColumns) {
			if (colIndex1Based > colCount) {
				throw std::runtime_error("Selected column out of bounds: " + std::to_string(colIndex1Based));
			}

			std::vector<std::string> values;
			values.reserve(rows.size());
			const size_t colIdx0 = static_cast<size_t>(colIndex1Based - 1);
			for (const auto& row : rows) {
				values.push_back(row[colIdx0]);
			}

			encodedColumns.push_back(encoder.encode(colIndex1Based, values));
		}

		// File header.
		std::ostream& out = std::cout;
		out.write("ACMP1", 5);
		write_u32(out, static_cast<uint32_t>(rows.size()));
		write_u32(out, static_cast<uint32_t>(colCount));
		write_u32(out, static_cast<uint32_t>(encodedColumns.size()));

		for (const auto& col : encodedColumns) {
			write_column(out, col);
		}

		out.flush();
		return 0;
	} catch (const std::exception& ex) {
		std::cerr << "Error: " << ex.what() << '\n';
		return 2;
	}
}

