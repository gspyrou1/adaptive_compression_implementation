#include "ac_bloom.h"

#include <algorithm>

namespace ac {

BloomFilter::BloomFilter() : numBits_(0), numHashes_(0) {}

BloomFilter::BloomFilter(uint32_t expectedItems, double falsePositiveRate) {
    if (expectedItems == 0) {
        numBits_   = 0;
        numHashes_ = 0;
        return;
    }

    // Optimal bit count:  m = -n * ln(p) / (ln 2)^2
    // Optimal hash count: k =  m / n * ln 2
    const double ln2 = 0.6931471805599453;
    const double m   = -static_cast<double>(expectedItems)
                       * std::log(falsePositiveRate) / (ln2 * ln2);

    numBits_   = static_cast<uint32_t>(m) + 1;
    numHashes_ = std::max(1u,
        static_cast<uint32_t>(m / static_cast<double>(expectedItems) * ln2 + 0.5));

    bits_.assign((numBits_ + 7) / 8, 0u);
}

bool BloomFilter::empty() const {
    return numBits_ == 0;
}

void BloomFilter::clear() {
    bits_.assign(bits_.size(), 0u);
}

// Two-hash FNV-1a variants give a well-distributed (h1, h2) pair cheaply.
std::pair<uint64_t, uint64_t> BloomFilter::hash_pair(const std::string& s) const {
    uint64_t h1 = 14695981039346656037ull;  // FNV-1a 64-bit offset basis
    uint64_t h2 = 0;
    for (unsigned char c : s) {
        h1 ^= static_cast<uint64_t>(c);
        h1 *= 1099511628211ull;             // FNV-1a 64-bit prime
        h2  = h2 * 31u + c;
    }
    return std::make_pair(h1, h2);
}

void BloomFilter::set_bit(uint32_t pos) {
    bits_[pos >> 3] |= static_cast<uint8_t>(1u << (pos & 7u));
}

bool BloomFilter::get_bit(uint32_t pos) const {
    return (bits_[pos >> 3] >> (pos & 7u)) & 1u;
}

void BloomFilter::insert(const std::string& s) {
    std::pair<uint64_t, uint64_t> hp = hash_pair(s);
    for (uint32_t i = 0; i < numHashes_; ++i) {
        const uint32_t pos = static_cast<uint32_t>(
            (hp.first + static_cast<uint64_t>(i) * hp.second) % numBits_);
        set_bit(pos);
    }
}

bool BloomFilter::mightContain(const std::string& s) const {
    if (numBits_ == 0) return false;
    std::pair<uint64_t, uint64_t> hp = hash_pair(s);
    for (uint32_t i = 0; i < numHashes_; ++i) {
        const uint32_t pos = static_cast<uint32_t>(
            (hp.first + static_cast<uint64_t>(i) * hp.second) % numBits_);
        if (!get_bit(pos)) return false;
    }
    return true;
}

} // namespace ac
