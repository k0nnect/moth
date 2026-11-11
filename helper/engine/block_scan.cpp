#include "engine/block_scan.hpp"

#include <cstring>

namespace moth {

namespace {

template <size_t W>
uint64_t load(const uint8_t* p) {
  uint64_t v = 0;
  std::memcpy(&v, p, W);  // little-endian host
  return v;
}

template <size_t W>
void scan_group(const uint8_t* data, size_t end, uint64_t base, const GroupSpec& g,
                std::vector<uint32_t>& out) {
  // first offset whose absolute address is a multiple of align
  size_t first = static_cast<size_t>((g.align - base % g.align) % g.align);
  const size_t step = g.align;
  const uint64_t xa = g.exclude_align;
  const Pred& p = g.pred;
  const ValueType t = g.type;
  if (p.op == Pred::Eq) {
    const uint64_t want = load<W>(reinterpret_cast<const uint8_t*>(&p.bits));
    for (size_t o = first; o < end; o += step) {
      if (load<W>(data + o) != want) continue;
      if (xa != 0 && (base + o) % xa == 0) continue;
      out.push_back(static_cast<uint32_t>(o));
    }
  } else {
    for (size_t o = first; o < end; o += step) {
      if (!matches(p, t, load<W>(data + o), 0)) continue;
      if (xa != 0 && (base + o) % xa == 0) continue;
      out.push_back(static_cast<uint32_t>(o));
    }
  }
}

}  // namespace

void scan_block(const uint8_t* data, size_t len, size_t limit, uint64_t base,
                std::span<const GroupSpec> groups, std::vector<std::vector<uint32_t>>& out) {
  for (const auto& g : groups) {
    if (g.pred.op != Pred::Eq && g.pred.op != Pred::Range)
      throw EngineError("bad_request", "previous-value predicate cannot scan a single buffer");
    if (g.align == 0) throw EngineError("bad_request", "align must be positive");
  }
  if (out.size() < groups.size()) out.resize(groups.size());
  for (size_t i = 0; i < groups.size(); i++) {
    const GroupSpec& g = groups[i];
    const size_t w = width_of(g.type);
    if (len < w) continue;
    const size_t end = limit < len - w + 1 ? limit : len - w + 1;  // o < limit and o + w <= len
    switch (w) {
      case 1: scan_group<1>(data, end, base, g, out[i]); break;
      case 2: scan_group<2>(data, end, base, g, out[i]); break;
      case 4: scan_group<4>(data, end, base, g, out[i]); break;
      default: scan_group<8>(data, end, base, g, out[i]); break;
    }
  }
}

}  // namespace moth
