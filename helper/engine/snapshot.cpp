#include "engine/snapshot.hpp"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace moth {

const uint8_t* Snapshot::at(uint64_t addr, size_t len) const {
  const size_t i = first_ending_after(addr);
  if (i == pieces_.size()) return nullptr;
  const Piece& p = pieces_[i];
  if (addr < p.base || addr + len > p.base + p.size) return nullptr;
  return p.data + (addr - p.base);
}

uint8_t* Snapshot::add_region(uint64_t size) {
  const size_t n = static_cast<size_t>(size);
  if (n != size) throw std::bad_alloc();             // more than this process can address
  std::unique_ptr<uint8_t[]> buffer(new uint8_t[n]);  // not zeroed: only what is read is held
  buffers_.push_back(std::move(buffer));
  bytes_ += size;
  last_size_ = size;
  last_first_piece_ = pieces_.size();
  return buffers_.back().get();
}

void Snapshot::hold(uint64_t base, uint64_t size, uint8_t* data) {
  if (size > 0) pieces_.push_back({base, size, data});
}

void Snapshot::end_region() {
  if (buffers_.empty() || pieces_.size() > last_first_piece_) return;
  buffers_.pop_back();
  bytes_ -= last_size_;
  last_size_ = 0;
}

void Snapshot::put(uint64_t addr, const void* src, size_t len) {
  const uint64_t end = addr + len;
  each_overlapping(addr, end, [&](const Piece& p) {
    const uint64_t lo = std::max(addr, p.base), hi = std::min(end, p.base + p.size);
    std::memcpy(p.data + (lo - p.base), static_cast<const uint8_t*>(src) + (lo - addr), static_cast<size_t>(hi - lo));
  });
}

void Snapshot::clear() {
  std::vector<Piece>().swap(pieces_);
  std::vector<std::unique_ptr<uint8_t[]>>().swap(buffers_);
  bytes_ = 0;
  last_size_ = 0;
  last_first_piece_ = 0;
}

size_t Snapshot::first_ending_after(uint64_t addr) const {
  auto it = std::partition_point(pieces_.begin(), pieces_.end(), [&](const Piece& p) { return p.base + p.size <= addr; });
  return static_cast<size_t>(it - pieces_.begin());
}

}  // namespace moth
