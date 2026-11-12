#pragma once
// Snapshot: a copy of a process's writable memory, which stands in for the previous value of every
// place a snapshot hunt still has, until a group is small enough to keep its own values (see
// Engine::snapshot). Portable C++20, no OS calls.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace moth {

class Engine;

// The bytes of the regions that were readable when it was taken. Each region is copied into a
// buffer of its own; the stretches of it that were read are held, and the rest (memory that could
// not be read) is not. Adding and replacing bytes is the engine's business.
class Snapshot {
 public:
  uint64_t bytes() const { return bytes_; }  // heap held: the buffers of the regions kept
  bool empty() const { return pieces_.empty(); }
  // The bytes saved for [addr, addr + len), or null if not all of them are held in one stretch.
  const uint8_t* at(uint64_t addr, size_t len) const;

 private:
  friend class Engine;

  // A stretch of held bytes, [base, base + size), saved at data.
  struct Piece {
    uint64_t base, size;
    uint8_t* data;
  };

  // A buffer for the next region's bytes, of size bytes and not yet held. Throws std::bad_alloc.
  uint8_t* add_region(uint64_t size);
  // Holds size bytes from base, saved at data in the last region's buffer. Pieces arrive in
  // ascending address order, apart.
  void hold(uint64_t base, uint64_t size, uint8_t* data);
  // Ends the last region: its buffer is freed if none of it is held.
  void end_region();
  // Calls f(piece) for each piece that overlaps [addr, end), ascending.
  template <class F> void each_overlapping(uint64_t addr, uint64_t end, F&& f) const {
    for (size_t i = first_ending_after(addr); i < pieces_.size() && pieces_[i].base < end; i++) f(pieces_[i]);
  }
  // Replaces the held bytes of [addr, addr + len) with len bytes from src; the rest are not held.
  void put(uint64_t addr, const void* src, size_t len);
  void clear();  // releases every byte

  size_t first_ending_after(uint64_t addr) const;  // the first piece that ends after addr

  std::vector<std::unique_ptr<uint8_t[]>> buffers_;  // one per region kept, ascending
  std::vector<Piece> pieces_;                        // ascending, apart
  uint64_t bytes_ = 0;
  uint64_t last_size_ = 0;       // the last region's buffer's size
  size_t last_first_piece_ = 0;  // its first piece
};

}  // namespace moth
