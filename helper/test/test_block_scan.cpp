#include <cfloat>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "test/test_util.hpp"

using namespace moth;
using namespace moth::test;
using enum ValueType;

TEST_CASE("aligned and unaligned i32") {
  Buf b(16); b.put<int32_t>(4, 100); b.put<int32_t>(9, 100);
  auto r = run(b, 16, 0, {G("a", I32, 4, 0, eq(100)), G("u", I32, 1, 4, eq(100))});
  CHECK(r[0] == V{4}); CHECK(r[1] == V{9});
}
TEST_CASE("limit and length") {
  // offsets 6 and 12 do not overlap; 12 is at the limit so only 6 is reported
  Buf b(16); b.put<int32_t>(6, 100); b.put<int32_t>(12, 100);
  CHECK(run(b, 12, 0, {G("u", I32, 1, 0, eq(100))})[0] == V{6});
  // a value starting just below the limit may extend past it
  Buf d(16); d.put<int32_t>(11, 100);
  CHECK(run(d, 12, 0, {G("u", I32, 1, 0, eq(100))})[0] == V{11});
  Buf c(16); c.put<int16_t>(14, 100);
  CHECK(run(c, 16, 0, {G("a", I32, 1, 0, eq(100))})[0].empty());
}
TEST_CASE("alignment is by absolute address") {
  Buf b(16); b.put<int32_t>(2, 100); b.put<int32_t>(8, 100);
  CHECK(run(b, 16, 0x1002, {G("a", I32, 4, 0, eq(100))})[0] == V{2});
}
TEST_CASE("float range is exclusive and ignores nan") {
  Buf b(24); float v[] = {99.0f, 99.5f, 100.0f, 100.9f, 101.0f, NAN};
  for (int i = 0; i < 6; i++) b.put<float>(i * 4, v[i]);
  CHECK(run(b, 24, 0, {G("f", F32, 4, 0, range(99, 101))})[0] == V{4, 8, 12});
}
TEST_CASE("other widths") {
  Buf b(32); b.put<uint8_t>(3, 30); b.put<int16_t>(6, -5); b.put<int64_t>(8, 100000); b.put<double>(16, 85.3);
  auto r = run(b, 32, 0, {G("b", U8, 1, 0, eq(30)), G("s", I16, 2, 0, eq(0xFFFB)),
                          G("q", I64, 8, 0, eq(100000)), G("d", F64, 8, 0, range(84, 86))});
  CHECK(r[0] == V{3}); CHECK(r[1] == V{6}); CHECK(r[2] == V{8}); CHECK(r[3] == V{16});
}
TEST_CASE("matches with previous value") {
  CHECK(matches({Pred::LtPrev}, ValueType::I32, bits<int32_t>(-1), bits<int32_t>(5)));
  CHECK_FALSE(matches({Pred::LtPrev}, ValueType::U32, bits<int32_t>(-1), bits<int32_t>(5)));
  CHECK(matches({Pred::GtPrev}, ValueType::F32, bits(2.5f), bits(1.0f)));
  CHECK_FALSE(matches({Pred::GtPrev}, ValueType::F32, bits((float)NAN), bits(1.0f)));
  CHECK(matches({Pred::EqPrev}, ValueType::F64, bits(1.5), bits(1.5)));
  CHECK(matches({Pred::NePrev}, ValueType::U8, 3, 4));
  Pred moved = range(-1, 10); moved.moved = true;                       // differs as ne_prev decides
  CHECK(matches(moved, F32, bits(2.0f), bits(1.0f))); CHECK_FALSE(matches(moved, F32, bits(2.0f), bits(2.0f)));
  CHECK_FALSE(matches(moved, F32, bits(0.0f), bits(-0.0f))); CHECK(matches(moved, F32, bits(2.0f), bits((float)NAN)));
  CHECK_FALSE(matches(moved, F32, bits(20.0f), bits(1.0f)));            // moved, out of range
}
TEST_CASE("a range with a floor skips tiny values and keeps exact zero") {
  Buf b(64);
  b.put<float>(0, 1.0f); b.put<int32_t>(4, 7);   // 7 read as a float is about 1e-44
  b.put<float>(8, 1e-7f); b.put<float>(12, 2e-6f); b.put<float>(16, 0.0f); b.put<float>(20, -0.0f);
  b.put<float>(24, -1e-7f); b.put<float>(28, -0.5f);
  CHECK(run(b, 32, 0, {G("f", F32, 4, 0, range_floor(0, 2, 1e-6))})[0] == V{0, 12});
  CHECK(run(b, 32, 0, {G("f", F32, 4, 0, range_floor(-1, 1, 1e-6))})[0] == V{12, 16, 20, 28});
  CHECK(run(b, 32, 0, {G("f", F32, 4, 0, range(0, 2))})[0] == V{0, 4, 8, 12});   // no floor: as before
}
TEST_CASE("scan_block rejects previous-value predicates") {
  Buf b(8);
  CHECK_THROWS_AS(run(b, 8, 0, {G("p", I32, 4, 0, Pred{Pred::LtPrev})}), EngineError);
  Pred moved = range(0, 2); moved.moved = true;
  CHECK_THROWS_AS(run(b, 8, 0, {G("p", F32, 4, 0, moved)}), EngineError);
}
TEST_CASE("type helpers") {
  ValueType t;
  CHECK(parse_type("f64", t)); CHECK(t == F64);
  CHECK_FALSE(parse_type("i33", t));
  CHECK(width_of(U16) == 2); CHECK(is_float(F32)); CHECK(is_signed(I8)); CHECK_FALSE(is_signed(U64));
}

// Beyond the brief: scan_block tests a range on integer keys, not with matches(); pin them together.

namespace {

// Bit patterns of type t a range test could get wrong: for 1- and 2-byte types all of them;
// otherwise extremes, NaNs and infinities, values at and next to each bound, and random bits.
std::vector<uint64_t> patterns(ValueType t, const std::vector<std::pair<double, double>>& ranges) {
  const size_t w = width_of(t);
  const uint64_t mask = w == 8 ? ~0ULL : (1ULL << (w * 8)) - 1, sign = 1ULL << (w * 8 - 1);
  std::vector<uint64_t> out;
  if (w <= 2) {
    for (uint64_t p = 0; p <= mask; p++) out.push_back(p);
    return out;
  }
  for (uint64_t p : {0ULL, 1ULL, mask, mask - 1, sign, sign - 1, sign + 1}) out.push_back(p);
  uint64_t x = 0x6d6f7468;
  for (int i = 0; i < 2000; i++) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    out.push_back(x & mask);
  }
  for (auto [lo, hi] : ranges) {
    for (double b : {lo, hi}) {
      if (!std::isfinite(b)) continue;
      if (t == F32) {
        float f = std::fabs(b) <= FLT_MAX ? static_cast<float>(b) : std::copysign(INFINITY, static_cast<float>(b));
        for (int i = 0; i < 3; i++) f = std::nextafterf(f, -INFINITY);
        for (int i = 0; i < 7; i++, f = std::nextafterf(f, INFINITY)) out.push_back(bits(f));
      } else if (t == F64) {
        double d = b;
        for (int i = 0; i < 3; i++) d = std::nextafter(d, -INFINITY);
        for (int i = 0; i < 7; i++, d = std::nextafter(d, INFINITY)) out.push_back(bits(d));
      } else {
        // integers around b; 64-bit ones far from zero round to doubles 1024 or 2048 apart
        const double top = std::ldexp(1.0, static_cast<int>(w * 8) - (is_signed(t) ? 1 : 0));
        if (!(b > (is_signed(t) ? -top : -1.0) && b < top)) continue;
        const uint64_t c = is_signed(t) ? static_cast<uint64_t>(static_cast<int64_t>(std::floor(b)))
                                        : static_cast<uint64_t>(std::floor(b));
        for (int64_t d = -1100; d <= 1100; d++) out.push_back((c + static_cast<uint64_t>(d)) & mask);
      }
    }
  }
  const float fs[] = {0.0f, -0.0f, INFINITY, -INFINITY, NAN, -NAN, FLT_MIN, FLT_TRUE_MIN, -FLT_TRUE_MIN, FLT_MAX, -FLT_MAX};
  const double ds[] = {0.0, -0.0, INFINITY, -INFINITY, NAN, -NAN, DBL_TRUE_MIN, -DBL_TRUE_MIN, DBL_MAX, -DBL_MAX};
  if (t == F32) {
    for (float f : fs) out.push_back(bits(f));
    out.push_back(0x7f800001);  // a signalling NaN
  }
  if (t == F64) {
    for (double d : ds) out.push_back(bits(d));
    out.push_back(0xfff0000000000001);  // a negative signalling NaN
  }
  return out;
}

}  // namespace

TEST_CASE("a range finds exactly the values matches() accepts, for every type") {
  const std::vector<std::pair<double, double>> ranges = {
      {99, 101}, {0.99, 1.01}, {-1, 1}, {-INFINITY, INFINITY}, {0, 0}, {1, -1}, {NAN, 1}, {-1, NAN},
      {1.5, 2.5}, {0.1, 0.30000000000000004}, {-0.0, 0.0}, {-1e300, 1e300}, {1e300, INFINITY},
      {-INFINITY, -1e300}, {-129, 128}, {-32769, 65536}, {-5, -2}, {1e-45, 1e-44},
      {-2147483648.5, 2147483647.5}, {4294967295.0, 4294967296.5}, {-9.3e18, 9.3e18},
      {1.8446744073709552e19, INFINITY}, {3.4028234663852886e38, INFINITY}};
  // with a floor: 1e-6 over the first four for floats, and beyond the brief, every type, a floor of
  // 2.5 and the range (-5, 5) too, so integer floors and a gap inside the range are covered
  const std::vector<std::pair<double, double>> floored = {{0, 2}, {-1, 1}, {-2, 0}, {5, 7}, {-5, 5}};
  const double floors[] = {1e-6, 2.5};
  for (ValueType t : {I8, U8, I16, U16, I32, U32, I64, U64, F32, F64}) {
    std::vector<std::pair<double, double>> bounds = ranges;
    bounds.insert(bounds.end(), floored.begin(), floored.end());
    for (double f : floors) bounds.push_back({-f, f});  // values next to the floor
    const std::vector<uint64_t> ps = patterns(t, bounds);
    const size_t w = width_of(t);
    Buf b(ps.size() * w);
    for (size_t i = 0; i < ps.size(); i++) std::memcpy(b.d.data() + i * w, &ps[i], w);  // little-endian host
    auto check = [&](const Pred& p) {
      V want;
      for (size_t i = 0; i < ps.size(); i++)
        if (matches(p, t, ps[i], 0)) want.push_back(static_cast<uint32_t>(i * w));
      const V got = run(b, b.d.size(), 0, {G("r", t, static_cast<uint32_t>(w), 0, p)})[0];
      INFO("type ", static_cast<int>(t), ", range (", p.lo, ", ", p.hi, "), floor ", p.floor);
      CHECK(got.size() == want.size());
      CHECK(got == want);
    };
    for (auto [lo, hi] : ranges) check(range(lo, hi));
    for (double f : floors)
      for (auto [lo, hi] : floored) check(range_floor(lo, hi, f));
  }
}
