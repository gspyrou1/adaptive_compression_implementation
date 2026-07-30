#include "ac_utils.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>

namespace ac {

uint8_t width_for_cardinality(uint32_t cardinality) {
    if (cardinality <= 0x100u)   return 1;
    if (cardinality <= 0x10000u) return 2;
    return 4;
}

bool is_numeric(const std::string& s) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    std::strtod(s.c_str(), &end);
    if (errno != 0) return false;
    return end != nullptr && *end == '\0';
}

bool is_string_column(const std::vector<std::string>& values) {
    for (const std::string& v : values) {
        if (!is_numeric(v)) return true;
    }
    return false;
}

uint64_t dictionary_storage_bytes(const std::vector<std::string>& values) {
    uint64_t bytes = 0;
    for (const std::string& v : values) {
        bytes += sizeof(uint32_t);                 // 4-byte length prefix
        bytes += static_cast<uint64_t>(v.size());  // string content
    }
    return bytes;
}

std::pair<std::string, std::string> min_max(const std::vector<std::string>& vals) {
    if (vals.empty()) return std::make_pair(std::string(""), std::string(""));
    auto mm = std::minmax_element(vals.begin(), vals.end());
    return std::make_pair(*mm.first, *mm.second);
}

} // namespace ac
