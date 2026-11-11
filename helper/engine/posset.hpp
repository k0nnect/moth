#pragma once
// PositionSet: the addresses one group matched, small enough to hold tens of millions.
// Positions are kept per scan block, either as the block's sorted 32-bit offsets or as a bitmap
// with one bit per align step, whichever takes fewer bytes. Portable C++20, no OS calls.
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace moth {

class PositionSet {
 public:
  struct Block {
    uint64_t base = 0;    // address of offset 0
    uint32_t span = 0;    // every offset is below span
    uint32_t count = 0;   // positions in this block, never 0
    bool bitmap = false;  // words: bit i is the i-th aligned offset; otherwise words are the offsets
    std::vector<uint32_t> words;
  };

  explicit PositionSet(uint32_t align) : align_(align) {}

  // Appends one block. Blocks arrive in ascending address order without overlapping. Offsets are
  // ascending, below span, and aligned to align by absolute address. An empty block is not stored.
  void add_block(uint64_t base, uint32_t span, std::span<const uint32_t> offsets);

  uint64_t count() const { return count_; }
  uint64_t bytes() const { return word_bytes_ + blocks_.capacity() * sizeof(Block); }  // heap held
  const std::vector<Block>& blocks() const { return blocks_; }

  // Calls f(offset) for each position of b, ascending.
  template <class F> void for_each_in(const Block& b, F&& f) const {
    if (!b.bitmap) {
      for (uint32_t o : b.words) f(o);
      return;
    }
    const uint32_t first = first_offset(b.base);
    for (size_t w = 0; w < b.words.size(); w++)
      for (uint32_t bits = b.words[w]; bits != 0; bits &= bits - 1) f(offset_of(first, w, bits));
  }

  // Calls f(addr) for every position, ascending.
  template <class F> void for_each(F&& f) const {
    for (const Block& b : blocks_) for_each_in(b, [&](uint32_t o) { f(b.base + o); });
  }

  // Addresses of positions [from, from + n) in ascending order; fewer past the end of the set.
  std::vector<uint64_t> addresses(uint64_t from, uint64_t n) const;

 private:
  // the first offset in a block at base whose absolute address is a multiple of align
  uint32_t first_offset(uint64_t base) const { return static_cast<uint32_t>((align_ - base % align_) % align_); }
  // the offset of the lowest set bit of bits, in bitmap word w
  uint32_t offset_of(uint32_t first, size_t w, uint32_t bits) const {
    return first + static_cast<uint32_t>((w * 32 + std::countr_zero(bits)) * align_);
  }

  uint32_t align_;
  uint64_t count_ = 0;
  uint64_t word_bytes_ = 0;
  std::vector<Block> blocks_;
};

}  // namespace moth
