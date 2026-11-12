#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/pred.hpp"

namespace moth {

// For each group, append ascending offsets o that match, where:
//   o < limit, o + width <= len, (base+o) % align == 0,
//   and exclude_align == 0 or (base+o) % exclude_align != 0.
// out is grown to groups.size() if smaller; existing entries are appended to.
// Previous-value predicates, and moved ranges, throw EngineError{"bad_request"}.
void scan_block(const uint8_t* data, size_t len, size_t limit, uint64_t base,
                std::span<const GroupSpec> groups, std::vector<std::vector<uint32_t>>& out);

}  // namespace moth
