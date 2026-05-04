#pragma once
#include "ac_types.h"

#include <cstdint>
#include <string>

namespace ac {

// Return the decoded string value at 0-based row index rowIndex.
//
// Walks pages forward, maintaining cumulative dictionary state, and stops
// as soon as the target row's page is reached. Only the pages up to and
// including the target page are touched -- rows after it are never read.
std::string random_access(const EncodedColumn& col, uint32_t rowIndex);

} // namespace ac
