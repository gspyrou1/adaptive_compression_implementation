#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace ac {

// Probabilistic membership filter (Bloom filter).
//
// Inserts strings into a compact bit array using k independent hash functions.
// mightContain() returns:
//   true  -- value is PROBABLY in the set (false positives possible)
//   false -- value is DEFINITELY NOT in the set (no false negatives)
//
// The false-positive rate is controlled at construction time.  The encoder
// uses a 10 % rate (matching the reference implementation) so that the filter
// slightly over-estimates cross-page repetition, making the algorithm
// appropriately conservative about resetting differential sequences.
class BloomFilter {
public:
    BloomFilter();

    // Build a filter sized for expectedItems elements at the given
    // false-positive probability (default 10 %).
    explicit BloomFilter(uint32_t expectedItems,
                         double   falsePositiveRate = 0.10);

    void insert(const std::string& s);
    bool mightContain(const std::string& s) const;

    // True if the filter has not been constructed with any capacity.
    bool empty() const;

    // Reset all bits without freeing memory.
    void clear();

private:
    // Double-hashing: two independent FNV-1a variants produce a pair of 64-bit
    // values; h_i(x) = (h1 + i*h2) mod numBits_ gives k hash positions.
    std::pair<uint64_t, uint64_t> hash_pair(const std::string& s) const;

    void set_bit(uint32_t pos);
    bool get_bit(uint32_t pos) const;

    std::vector<uint8_t> bits_;
    uint32_t             numBits_;
    uint32_t             numHashes_;
};

} // namespace ac
