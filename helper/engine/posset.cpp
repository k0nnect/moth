#include "engine/posset.hpp"

#include <utility>

namespace moth {

void PositionSet::add_block(uint64_t base, uint32_t span, std::span<const uint32_t> offsets) {
  if (offsets.empty()) return;
  Block b;
  b.base = base;
  b.span = span;
  b.count = static_cast<uint32_t>(offsets.size());
  const uint32_t first = first_offset(base);
  const uint64_t slots = span > first ? (uint64_t{span} - first + align_ - 1) / align_ : 0;
  const uint64_t nwords = (slots + 31) / 32;
  if (nwords < offsets.size()) {
    b.bitmap = true;
    b.words.assign(static_cast<size_t>(nwords), 0);
    for (uint32_t o : offsets) {
      const uint32_t slot = (o - first) / align_;
      b.words[slot / 32] |= 1u << (slot % 32);
    }
  } else {
    b.words.assign(offsets.begin(), offsets.end());
  }
  count_ += b.count;
  word_bytes_ += b.words.capacity() * sizeof(uint32_t);
  blocks_.push_back(std::move(b));
}

std::vector<uint64_t> PositionSet::addresses(uint64_t from, uint64_t n) const {
  std::vector<uint64_t> out;
  if (from >= count_ || n == 0) return out;
  const size_t want = static_cast<size_t>(n < count_ - from ? n : count_ - from);
  out.reserve(want);
  for (const Block& b : blocks_) {
    if (out.size() == want) break;
    if (from >= b.count) {  // skip the whole block
      from -= b.count;
      continue;
    }
    if (!b.bitmap) {
      for (size_t i = static_cast<size_t>(from); i < b.count && out.size() < want; i++)
        out.push_back(b.base + b.words[i]);
    } else {
      const uint32_t first = first_offset(b.base);
      for (size_t w = 0; w < b.words.size() && out.size() < want; w++) {
        uint32_t bits = b.words[w];
        const unsigned here = static_cast<unsigned>(std::popcount(bits));
        if (from >= here) {  // skip the whole word
          from -= here;
          continue;
        }
        for (; bits != 0 && out.size() < want; bits &= bits - 1) {
          if (from > 0) {
            from--;
            continue;
          }
          out.push_back(b.base + offset_of(first, w, bits));
        }
      }
    }
    from = 0;
  }
  return out;
}

}  // namespace moth
