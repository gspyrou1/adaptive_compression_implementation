#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Small helper functions shared across encoding, decoding, and scanning.

namespace ac {

// Returns the number of bytes needed to store an index into a dictionary
// of the given cardinality (number of entries).
uint8_t width_for_cardinality(uint32_t cardinality);

// Returns true if the string can be fully parsed as a number (int or float).
bool is_numeric(const std::string& s);

// Returns true if the column contains at least one non-numeric value.
bool is_string_column(const std::vector<std::string>& values);

// Returns the byte size of a sorted dictionary stored on disk.
// Each entry is serialised as: 4-byte length prefix + string content.
uint64_t dictionary_storage_bytes(const std::vector<std::string>& values);

// Returns (min, max) of a vector of strings using lexicographic order.
// Returns ("", "") for an empty vector.
std::pair<std::string, std::string> min_max(const std::vector<std::string>& vals);

} // namespace ac
