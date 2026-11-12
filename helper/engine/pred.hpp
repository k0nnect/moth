#pragma once
#include <cstdint>
#include <string>

#include "engine/types.hpp"

namespace moth {

struct Pred {
  enum Op { Eq, Range, LtPrev, GtPrev, EqPrev, NePrev } op;
  uint64_t bits = 0;       // Eq: little-endian value bytes in the low bits
  double lo = 0, hi = 0;   // Range: lo < value < hi, exclusive
  // Range only. floor above 0: the value must also be exactly zero (either sign) or have a
  // magnitude of at least floor; 0, or anything not above 0, means no floor. moved: the value must
  // also differ from prev as NePrev decides, so a moved range compares with the last value.
  double floor = 0;
  bool moved = false;
};

struct GroupSpec {
  std::string id;
  ValueType type;
  uint32_t align;
  uint32_t exclude_align;  // 0 means no exclusion
  Pred pred;
};

// cur and prev hold the value's bytes little-endian in the low bits.
bool matches(const Pred& p, ValueType t, uint64_t cur, uint64_t prev);

}  // namespace moth
