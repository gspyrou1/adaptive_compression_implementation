#pragma once
#include "ac_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ac {

// Return the decoded string value at 0-based row index rowIndex.
//
// Walks pages forward, maintaining cumulative dictionary state, and stops
// as soon as the target row's page is reached. Only the pages up to and
// including the target page are touched -- rows after it are never read.
std::string random_access(const EncodedColumn& col, uint32_t rowIndex);

// Fetch decoded values for a sorted list of 0-based row indices (§3.2.4).
//
// rowIndices must be sorted ascending (this matches the output of filtered_scan).
// Result[i] is the decoded value for rowIndices[i].
//
// More efficient than calling random_access() once per index: the cumulative
// dictionary is maintained incrementally as pages are walked sequentially,
// so it is rebuilt at most once per touched page regardless of how many
// indices fall within it.
std::vector<std::string> batch_random_access(const EncodedColumn& col,
                                              const std::vector<uint32_t>& rowIndices);

} // namespace ac
