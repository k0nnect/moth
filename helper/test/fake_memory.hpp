#pragma once
// Test-only ProcessMemory: zero-filled regions keyed by base address, with switches for the
// ways real memory fails (unreadable, read-only, freed, process gone).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

#include "engine/memory.hpp"

namespace moth::test {

class FakeMemory : public ProcessMemory {
 public:
  void add_region(uint64_t base, size_t size) { regions_[base] = Reg{base, std::vector<uint8_t>(size, 0)}; }
  void remove_region(uint64_t base) { regions_.erase(base); }
  void set_unreadable(uint64_t base, bool v) { regions_.at(base).unreadable = v; }
  void set_readonly(uint64_t base, bool v) { regions_.at(base).readonly = v; }
  void set_alive(bool v) { alive_ = v; }

  // Direct access for setting up and checking a test; ignores every switch.
  template <class T> void poke(uint64_t addr, T v) { std::memcpy(at(addr, sizeof(T)), &v, sizeof(T)); }
  template <class T> T peek(uint64_t addr) {
    T v;
    std::memcpy(&v, at(addr, sizeof(T)), sizeof(T));
    return v;
  }

  size_t read_calls = 0;
  uint64_t bytes_read = 0;

  // Read-only regions are not writable, so they are not listed.
  std::vector<Region> writable_regions() override {
    std::vector<Region> out;
    if (!alive_) return out;
    for (const auto& [base, r] : regions_)
      if (!r.readonly) out.push_back({base, r.bytes.size()});
    return out;
  }

  // Reads stop at the end of the region holding addr, like a partial ReadProcessMemory.
  size_t read(uint64_t addr, void* out, size_t len) override {
    read_calls++;
    Reg* r = holding(addr);
    if (!alive_ || r == nullptr || r->unreadable) return 0;
    const size_t off = static_cast<size_t>(addr - r->base);
    const size_t n = len < r->bytes.size() - off ? len : r->bytes.size() - off;
    std::memcpy(out, r->bytes.data() + off, n);
    bytes_read += n;
    return n;
  }

  bool write(uint64_t addr, const void* data, size_t len) override {
    Reg* r = holding(addr);
    if (!alive_ || r == nullptr || r->unreadable || r->readonly) return false;
    const size_t off = static_cast<size_t>(addr - r->base);
    if (len > r->bytes.size() - off) return false;
    std::memcpy(r->bytes.data() + off, data, len);
    return true;
  }

  bool alive() override { return alive_; }

 private:
  struct Reg {
    uint64_t base = 0;
    std::vector<uint8_t> bytes;
    bool unreadable = false, readonly = false;
  };

  // The region containing addr, or null.
  Reg* holding(uint64_t addr) {
    auto it = regions_.upper_bound(addr);
    if (it == regions_.begin()) return nullptr;
    --it;
    return addr - it->first < it->second.bytes.size() ? &it->second : nullptr;
  }

  uint8_t* at(uint64_t addr, size_t len) {
    Reg* r = holding(addr);
    if (r == nullptr || len > r->bytes.size() - (addr - r->base)) throw std::out_of_range("fake memory: no region there");
    return r->bytes.data() + (addr - r->base);
  }

  std::map<uint64_t, Reg> regions_;
  bool alive_ = true;
};

// Fills [base, base + size) with xorshift64 output from seed, the same bytes for the same seed.
// The range must lie in one writable region.
inline void fill_xorshift(FakeMemory& m, uint64_t base, size_t size, uint64_t seed) {
  uint64_t x = seed != 0 ? seed : 1;
  std::vector<uint8_t> chunk(size_t{1} << 20);
  for (size_t done = 0; done < size; done += chunk.size()) {
    const size_t n = size - done < chunk.size() ? size - done : chunk.size();
    for (size_t i = 0; i < n; i += 8) {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      std::memcpy(chunk.data() + i, &x, n - i < 8 ? n - i : 8);
    }
    if (!m.write(base + done, chunk.data(), n)) throw std::out_of_range("fake memory: cannot fill there");
  }
}

}  // namespace moth::test
