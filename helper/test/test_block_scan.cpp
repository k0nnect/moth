#include <cmath>

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
}
TEST_CASE("scan_block rejects previous-value predicates") {
  Buf b(8);
  CHECK_THROWS_AS(run(b, 8, 0, {G("p", I32, 4, 0, Pred{Pred::LtPrev})}), EngineError);
}
TEST_CASE("type helpers") {
  ValueType t;
  CHECK(parse_type("f64", t)); CHECK(t == F64);
  CHECK_FALSE(parse_type("i33", t));
  CHECK(width_of(U16) == 2); CHECK(is_float(F32)); CHECK(is_signed(I8)); CHECK_FALSE(is_signed(U64));
}
