#pragma once
// Small helpers shared by the engine tests.
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "engine/block_scan.hpp"

namespace moth::test {

using V = std::vector<uint32_t>;

struct Buf {
  std::vector<uint8_t> d;
  explicit Buf(size_t n) : d(n, 0) {}
  template <class T> void put(size_t off, T v) { std::memcpy(d.data() + off, &v, sizeof(T)); }
};

// value bytes in the low bits of a uint64_t, as scan_block and matches expect
template <class T> uint64_t bits(T v) {
  uint64_t r = 0;
  std::memcpy(&r, &v, sizeof(T));
  return r;
}

inline Pred eq(uint64_t b) { Pred p{Pred::Eq}; p.bits = b; return p; }
inline Pred range(double lo, double hi) { Pred p{Pred::Range}; p.lo = lo; p.hi = hi; return p; }

inline GroupSpec G(std::string id, ValueType t, uint32_t align, uint32_t excl, Pred p) {
  return GroupSpec{std::move(id), t, align, excl, p};
}

inline std::vector<V> run(const Buf& b, size_t limit, uint64_t base, std::initializer_list<GroupSpec> gs) {
  std::vector<GroupSpec> v(gs);
  std::vector<V> out;
  scan_block(b.d.data(), b.d.size(), limit, base, v, out);
  out.resize(v.size());
  return out;
}

}  // namespace moth::test
