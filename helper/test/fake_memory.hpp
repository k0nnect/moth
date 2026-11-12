#pragma once
// Test-only ProcessMemory: zero-filled regions keyed by base address, with switches for the
// ways real memory fails (unreadable, read-only, freed, process gone).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#include "engine/memory.hpp"

namespace moth::test {

class FakeMemory : public ProcessMemory {
 public:
  static constexpr uint64_t kPage = 4096;

  void add_region(uint64_t base, size_t size) { regions_[base] = Reg{base, std::vector<uint8_t>(size, 0)}; }
  void remove_region(uint64_t base) { regions_.erase(base); }
  void set_unreadable(uint64_t base, bool v) { regions_.at(base).unreadable = v; }
  void set_readonly(uint64_t base, bool v) { regions_.at(base).readonly = v; }
  void set_alive(bool v) { alive_ = v; }
  // The page holding addr can no longer be read or written, while its region stays listed, as
  // when a game decommits one page of a larger allocation after the regions were walked.
  void free_page(uint64_t addr) { freed_.insert(addr - addr % kPage); }
  // On: a read that cannot get every byte it asks for gets none, as ReadProcessMemory does, and
  // a read runs on into a region that starts where the last one ends. Off (the default): a read
  // stops at the end of the region holding addr, or at a freed page, and returns what it got.
  void set_all_or_nothing(bool v) { all_or_nothing_ = v; }

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

  // Every region but unreadable ones, read-only ones included. A freed page stays inside its region.
  std::vector<Region> readable_regions() override {
    std::vector<Region> out;
    if (!alive_) return out;
    for (const auto& [base, r] : regions_)
      if (!r.unreadable) out.push_back({base, r.bytes.size()});
    return out;
  }

  size_t read(uint64_t addr, void* out, size_t len) override {
    read_calls++;
    if (!alive_) return 0;
    size_t n = readable(addr, len, all_or_nothing_);
    if (all_or_nothing_ && n < len) n = 0;
    for (size_t done = 0; done < n;) {  // region by region
      Reg* r = holding(addr + done);
      const size_t off = static_cast<size_t>(addr + done - r->base);
      const size_t k = n - done < r->bytes.size() - off ? n - done : r->bytes.size() - off;
      std::memcpy(static_cast<uint8_t*>(out) + done, r->bytes.data() + off, k);
      done += k;
    }
    bytes_read += n;
    return n;
  }

  bool write(uint64_t addr, const void* data, size_t len) override {
    Reg* r = holding(addr);
    if (!alive_ || r == nullptr || r->unreadable || r->readonly) return false;
    const size_t off = static_cast<size_t>(addr - r->base);
    if (len > r->bytes.size() - off || readable(addr, len, false) < len) return false;
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

  // How many bytes from addr, up to len, can be read in a row: to the end of the region holding
  // addr (on through adjacent regions when across), stopping at an unreadable region or a freed page.
  size_t readable(uint64_t addr, size_t len, bool across) {
    size_t n = 0;
    while (n < len) {
      const uint64_t a = addr + n;
      Reg* r = holding(a);
      if (r == nullptr || r->unreadable) break;
      const uint64_t left = r->base + r->bytes.size() - a;
      size_t here = len - n < left ? len - n : static_cast<size_t>(left);
      auto f = freed_.lower_bound(a - a % kPage);  // the first freed page that ends after a
      if (f != freed_.end() && *f < a + here) return n + static_cast<size_t>(*f > a ? *f - a : 0);
      n += here;
      if (!across) break;
    }
    return n;
  }

  uint8_t* at(uint64_t addr, size_t len) {
    Reg* r = holding(addr);
    if (r == nullptr || len > r->bytes.size() - (addr - r->base)) throw std::out_of_range("fake memory: no region there");
    return r->bytes.data() + (addr - r->base);
  }

  std::map<uint64_t, Reg> regions_;
  std::set<uint64_t> freed_;  // page bases
  bool alive_ = true;
  bool all_or_nothing_ = false;
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
