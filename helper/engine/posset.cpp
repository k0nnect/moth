#include "engine/posset.hpp"

#include <utility>

namespace moth {

PositionSet::Builder::Builder(uint32_t align, uint64_t base, uint32_t span)
    : align_(align), first_(first_offset(align, base)) {
  const uint64_t slots = span > first_ ? (uint64_t{span} - first_ + align - 1) / align : 0;
  nwords_ = static_cast<size_t>((slots + 31) / 32);
  block_.base = base;
  block_.span = span;
}

// Once the list holds more offsets than the bitmap has words, the bitmap is smaller.
void PositionSet::Builder::to_bitmap() {
  std::vector<uint32_t> offsets;
  offsets.swap(block_.words);
  block_.words.assign(nwords_, 0);
  block_.bitmap = true;
  for (uint32_t o : offsets) set_bit(o);
}

PositionSet::Block PositionSet::Builder::finish() {
  // a list grown one offset at a time may have spare room; storage counts capacity, so trim it
  if (!block_.bitmap && block_.words.capacity() != block_.words.size())
    block_.words = std::vector<uint32_t>(block_.words.begin(), block_.words.end());
  return std::move(block_);
}

void PositionSet::add_block(uint64_t base, uint32_t span, std::span<const uint32_t> offsets) {
  Builder b(align_, base, span);
  b.add(offsets, 0);
  append(b.finish());
}

void PositionSet::append(Block b) {
  if (b.count == 0) return;
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
