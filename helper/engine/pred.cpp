#include "engine/pred.hpp"

#include <cmath>
#include <cstring>

namespace moth {

size_t width_of(ValueType t) {
  switch (t) {
    case ValueType::I8: case ValueType::U8: return 1;
    case ValueType::I16: case ValueType::U16: return 2;
    case ValueType::I32: case ValueType::U32: case ValueType::F32: return 4;
    default: return 8;
  }
}

bool is_float(ValueType t) { return t == ValueType::F32 || t == ValueType::F64; }

bool is_signed(ValueType t) {
  switch (t) {
    case ValueType::I8: case ValueType::I16: case ValueType::I32: case ValueType::I64:
    case ValueType::F32: case ValueType::F64: return true;
    default: return false;
  }
}

bool parse_type(std::string_view s, ValueType& out) {
  static const struct { std::string_view name; ValueType t; } names[] = {
      {"i8", ValueType::I8},   {"u8", ValueType::U8},   {"i16", ValueType::I16}, {"u16", ValueType::U16},
      {"i32", ValueType::I32}, {"u32", ValueType::U32}, {"i64", ValueType::I64}, {"u64", ValueType::U64},
      {"f32", ValueType::F32}, {"f64", ValueType::F64}};
  for (const auto& n : names)
    if (n.name == s) { out = n.t; return true; }
  return false;
}

namespace {

uint64_t mask_of(size_t w) { return w == 8 ? ~0ULL : (1ULL << (w * 8)) - 1; }

// Decode the low bytes of v as a number: signed ints sign-extend, floats convert.
double as_double(ValueType t, uint64_t v) {
  switch (t) {
    case ValueType::I8: return static_cast<int8_t>(v);
    case ValueType::U8: return static_cast<uint8_t>(v);
    case ValueType::I16: return static_cast<int16_t>(v);
    case ValueType::U16: return static_cast<uint16_t>(v);
    case ValueType::I32: return static_cast<int32_t>(v);
    case ValueType::U32: return static_cast<uint32_t>(v);
    case ValueType::I64: return static_cast<double>(static_cast<int64_t>(v));
    case ValueType::U64: return static_cast<double>(v);
    case ValueType::F32: { uint32_t b = static_cast<uint32_t>(v); float f; std::memcpy(&f, &b, 4); return f; }
    case ValueType::F64: { double d; std::memcpy(&d, &v, 8); return d; }
  }
  return 0;
}

// -1, 0, 1, or 2 when unordered (NaN). Integers compare exactly.
int compare(ValueType t, uint64_t a, uint64_t b) {
  if (is_float(t)) {
    double x = as_double(t, a), y = as_double(t, b);
    if (x != x || y != y) return 2;
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  if (is_signed(t)) {
    int64_t x, y;
    switch (width_of(t)) {
      case 1: x = static_cast<int8_t>(a); y = static_cast<int8_t>(b); break;
      case 2: x = static_cast<int16_t>(a); y = static_cast<int16_t>(b); break;
      case 4: x = static_cast<int32_t>(a); y = static_cast<int32_t>(b); break;
      default: x = static_cast<int64_t>(a); y = static_cast<int64_t>(b); break;
    }
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  uint64_t m = mask_of(width_of(t));
  a &= m; b &= m;
  return a < b ? -1 : (a > b ? 1 : 0);
}

}  // namespace

bool matches(const Pred& p, ValueType t, uint64_t cur, uint64_t prev) {
  uint64_t m = mask_of(width_of(t));
  switch (p.op) {
    case Pred::Eq: return (cur & m) == (p.bits & m);
    case Pred::Range: {
      double v = as_double(t, cur);
      if (!(v > p.lo && v < p.hi)) return false;  // NaN fails both
      if (p.floor > 0 && v != 0 && std::fabs(v) < p.floor) return false;
      return !p.moved || compare(t, cur, prev) != 0;
    }
    case Pred::LtPrev: return compare(t, cur, prev) == -1;
    case Pred::GtPrev: return compare(t, cur, prev) == 1;
    case Pred::EqPrev: return compare(t, cur, prev) == 0;
    case Pred::NePrev: { int c = compare(t, cur, prev); return c != 0; }
  }
  return false;
}

}  // namespace moth
