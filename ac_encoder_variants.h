#pragma once

#include "ac_types.h"

#include <cstdint>
#include <string>
#include <vector>

// Encoder variants for benchmarking individual optimizations.
//
// Each variant is a drop-in replacement for AdaptiveDictionaryEncoder.
// Only the named optimization is applied; everything else is identical
// to the baseline so the performance difference is isolated.
//
//   EncoderC1   -- C1 only: eliminate the page copy
//   EncoderC2   -- C2 only: defer localIndex / localOffsets until needed
//   EncoderC1C2 -- C1 + C2 combined

namespace ac {

struct EncoderC1 {
    EncodedColumn encode(uint32_t colIndex1Based,
                         const std::vector<std::string>& columnValues);
};

struct EncoderC2 {
    EncodedColumn encode(uint32_t colIndex1Based,
                         const std::vector<std::string>& columnValues);
};

struct EncoderC1C2 {
    EncodedColumn encode(uint32_t colIndex1Based,
                         const std::vector<std::string>& columnValues);
};

} // namespace ac
