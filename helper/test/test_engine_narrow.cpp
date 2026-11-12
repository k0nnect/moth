#include <cmath>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "engine/engine.hpp"
#include "test/fake_memory.hpp"
#include "test/test_util.hpp"

using namespace moth;
using namespace moth::test;
using enum ValueType;

static void three(FakeMemory& m) { m.add_region(0x10000, 4096); for (int i = 0; i < 3; i++) m.poke<int32_t>(0x10000 + i * 16, 100); }
TEST_CASE("narrow by a new number") {
  FakeMemory m; three(m); Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s); m.poke<int32_t>(0x10010, 85);
  CHECK(e.narrow({{"a", eq(85)}}, true, g, s)); CHECK(g[0].count == 1); CHECK(e.peek("a", 0, 9)[0].addr == 0x10010);
}
TEST_CASE("atomic narrow that would empty everything changes nothing") {
  FakeMemory m; three(m); Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  CHECK_FALSE(e.narrow({{"a", eq(77)}}, true, g, s)); CHECK(e.groups()[0].count == 3);
  CHECK(e.narrow({{"a", eq(77)}}, false, g, s)); CHECK(e.groups()[0].count == 0);
}
TEST_CASE("relations compare with the last applied step") {
  FakeMemory m; three(m); Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.poke<int32_t>(0x10000, 90); m.poke<int32_t>(0x10010, 110);
  CHECK(e.narrow({{"a", {Pred::NePrev}}}, true, g, s)); CHECK(g[0].count == 2);
  m.poke<int32_t>(0x10000, 80);                                         // down again from 90
  CHECK(e.narrow({{"a", {Pred::LtPrev}}}, true, g, s)); CHECK(g[0].count == 1);
  CHECK(e.peek("a", 0, 9)[0].addr == 0x10000);
}
TEST_CASE("relation without stored values is refused, then allowed once small") {
  FakeMemory m; three(m); Engine e(m, {.value_cap = 2}); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  CHECK_THROWS_WITH_AS(e.narrow({{"a", {Pred::LtPrev}}}, true, g, s), doctest::Contains("no_values"), EngineError);
  m.poke<int32_t>(0x10000, 85); m.poke<int32_t>(0x10010, 85);
  CHECK(e.narrow({{"a", eq(85)}}, true, g, s)); CHECK(g[0].has_values);
  m.poke<int32_t>(0x10000, 60); CHECK(e.narrow({{"a", {Pred::LtPrev}}}, true, g, s)); CHECK(g[0].count == 1);
}
TEST_CASE("a moved range drops values that pass the range without having changed") {
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<float>(0x10000, 5.0f); m.poke<float>(0x10010, 5.5f);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("f", F32, 4, 0, range(4, 6))}, false, s); CHECK(e.groups()[0].count == 2);
  m.poke<float>(0x10000, 6.0f);
  Pred p = range(5, 7); p.moved = true;
  CHECK(e.narrow({{"f", p}}, true, g, s)); CHECK(g[0].count == 1); CHECK(e.peek("f", 0, 9)[0].addr == 0x10000);
}
TEST_CASE("moved needs stored values and cannot start a scan") {
  FakeMemory m; three(m); Engine e(m, {.value_cap = 2}); ScanStats s; std::vector<GroupInfo> g;
  Pred p = range(99, 101); p.moved = true;
  CHECK_THROWS_WITH_AS(e.scan({G("a", I32, 4, 0, p)}, false, s), doctest::Contains("bad_request"), EngineError);
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  CHECK_THROWS_WITH_AS(e.narrow({{"a", p}}, true, g, s), doctest::Contains("no_values"), EngineError);
}

namespace {

// One type's stored values through a scan, a moved narrow, relation narrows and a write. Three
// places hold start; then the first goes up and the second down. Needs written < down < start < up.
template <class T> void stored_values_hold(ValueType t, T start, T up, T down, T written) {
  FakeMemory m; m.add_region(0x10000, 4096); for (int i = 0; i < 3; i++) m.poke<T>(0x10000 + i * 16, start);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g; uint64_t prev = 0;
  auto sc = e.scan({G("v", t, sizeof(T), 0, eq(bits(start)))}, false, s);
  REQUIRE(sc[0].count == 3); REQUIRE(sc[0].has_values);
  m.poke<T>(0x10000, up); m.poke<T>(0x10010, down);
  Pred moved = range(-1e9, 1e9); moved.moved = true;
  CHECK(e.narrow({{"v", moved}}, true, g, s)); CHECK(g[0].count == 2);             // the third still holds start
  CHECK(e.narrow({{"v", {Pred::EqPrev}}}, true, g, s)); CHECK(g[0].count == 2);   // up and down were stored
  REQUIRE(e.write_value(0x10010, t, bits(written), prev)); CHECK(prev == bits(down));
  CHECK(m.peek<T>(0x10010) == written);
  m.poke<T>(0x10000, down);                                             // down from up; the second holds what was written
  CHECK(e.narrow({{"v", {Pred::LtPrev}}}, true, g, s)); CHECK(g[0].count == 1);
  CHECK(e.peek("v", 0, 9)[0].addr == 0x10000);
}

}  // namespace

TEST_CASE("stored values of every width hold through a scan, a narrow and a write") {
  SUBCASE("u8") { stored_values_hold<uint8_t>(U8, 200, 250, 7, 3); }
  SUBCASE("i16") { stored_values_hold<int16_t>(I16, -300, -2, -30000, -30001); }
  SUBCASE("f32") { stored_values_hold<float>(F32, 100.0f, std::nextafter(100.0f, 200.0f), -0.5f, -1e30f); }
  SUBCASE("f64") { stored_values_hold<double>(F64, 100.0, std::nextafter(100.0, 200.0), -0.5, -1e300); }
}
TEST_CASE("a group over value_cap has no values at any width until a narrow brings it under") {
  // each place in its own block: values are stored for the first two and dropped at the third
  FakeMemory m; three(m); ScanStats s; std::vector<GroupInfo> g;
  const std::vector<GroupSpec> gs = {G("b", U8, 1, 0, eq(100)), G("h", I16, 2, 0, eq(100)),
                                     G("a", I32, 4, 0, eq(100)), G("q", I64, 8, 0, eq(100))};
  Engine e(m, {.value_cap = 2, .block_size = 16}), none(m, {.value_cap = 0, .block_size = 16});
  for (const GroupInfo& i : e.scan(gs, false, s)) { CHECK(i.count == 3); CHECK(i.tracked); CHECK_FALSE(i.has_values); }
  none.scan(gs, false, s); CHECK(e.storage_bytes() == none.storage_bytes());   // the dropped values are freed
  Pred moved = range(99, 101); moved.moved = true;
  for (const GroupSpec& sp : gs) {
    CHECK_THROWS_WITH_AS(e.narrow({{sp.id, {Pred::LtPrev}}}, true, g, s), doctest::Contains("no_values"), EngineError);
    CHECK_THROWS_WITH_AS(e.narrow({{sp.id, moved}}, true, g, s), doctest::Contains("no_values"), EngineError);
  }
  m.poke<int32_t>(0x10000, 85);
  CHECK(e.narrow({{"b", eq(100)}, {"h", eq(100)}, {"a", eq(100)}, {"q", eq(100)}}, true, g, s));
  for (const GroupInfo& i : g) { CHECK(i.count == 2); CHECK(i.has_values); }
  m.poke<int32_t>(0x10010, 60);
  CHECK(e.narrow({{"b", {Pred::LtPrev}}, {"h", {Pred::LtPrev}}, {"a", {Pred::LtPrev}}, {"q", {Pred::LtPrev}}}, true, g, s));
  for (const GroupInfo& i : g) CHECK(i.count == 1);
  for (const GroupSpec& sp : gs) CHECK(e.peek(sp.id, 0, 9)[0].addr == 0x10010);
}
TEST_CASE("freed memory loses candidates instead of failing") {
  FakeMemory m; three(m); m.add_region(0x50000, 4096); m.poke<int32_t>(0x50000, 100);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.remove_region(0x50000); CHECK(e.narrow({{"a", eq(100)}}, true, g, s)); CHECK(g[0].count == 3); CHECK(s.lost == 1);
}
TEST_CASE("a freed range is skipped without a read for every page") {
  const uint64_t big = 16u << 20;
  FakeMemory m; m.set_all_or_nothing(true);
  m.add_region(0x100000, big); m.add_region(0x100000 + big, 4096);
  for (uint64_t a = 0x100000; a < 0x100000 + big; a += 4096) m.poke<int32_t>(a, 7);
  m.poke<int32_t>(0x100000 + big, 7);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(7))}, false, s); CHECK(e.groups()[0].count == 4097);
  m.remove_region(0x100000);   // the game frees the whole 16 MiB
  m.read_calls = 0;
  CHECK(e.narrow({{"a", eq(7)}}, true, g, s)); CHECK(g[0].count == 1); CHECK(s.lost == 4096); CHECK(m.read_calls <= 16);
}
TEST_CASE("sparse candidates are read in runs, not whole blocks") {
  FakeMemory m; m.add_region(0x1000000, 12u << 20);
  m.poke<int32_t>(0x1000000, 100); m.poke<int32_t>(0x1000064, 100); m.poke<int32_t>(0x1000000 + (10u << 20), 100);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.read_calls = 0; m.bytes_read = 0; e.narrow({{"a", eq(100)}}, true, g, s);
  CHECK(m.read_calls == 2); CHECK(m.bytes_read < (1u << 20));
}
TEST_CASE("the game closing is reported") {
  FakeMemory m; three(m); Engine e(m); ScanStats s; std::vector<GroupInfo> g; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.set_alive(false); CHECK_THROWS_WITH_AS(e.narrow({{"a", eq(1)}}, true, g, s), doctest::Contains("target_gone"), EngineError);
}
TEST_CASE("read, write and peek paging") {
  FakeMemory m; three(m); Engine e(m); ScanStats s; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  uint64_t prev = 0, cur = 0; CHECK(e.write_value(0x10000, ValueType::I32, 999, prev)); CHECK(prev == 100);
  CHECK(e.read_value(0x10000, ValueType::I32, cur)); CHECK(cur == 999);
  CHECK(e.peek("a", 1, 1).size() == 1); CHECK(e.peek("a", 1, 1)[0].addr == 0x10010);
  m.set_readonly(0x10000, true); CHECK_FALSE(e.write_value(0x10000, ValueType::I32, 1, prev));
  e.drop({"a"}); CHECK(e.groups().empty());
}
TEST_CASE("peek_from starts at an address") {
  FakeMemory m; three(m); Engine e(m); ScanStats s;
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  auto items = e.peek_from("a", 0x10004, 9); CHECK(items.size() == 2); CHECK(items[0].addr == 0x10010);
  CHECK(e.peek_from("a", 0x20000, 9).empty());
}

// Beyond the brief: the error format, and the choices the brief leaves to the engine.

namespace {

// FakeMemory whose game closes when a read finds reads_left at zero.
struct DyingMemory : FakeMemory {
  int reads_left = 1 << 30;
  size_t read(uint64_t addr, void* out, size_t len) override {
    if (reads_left-- <= 0) set_alive(false);
    return FakeMemory::read(addr, out, len);
  }
};

}  // namespace

TEST_CASE("an engine error reads its code, then its message") {
  EngineError err("no_values", "group a has no stored values");
  CHECK(std::string(err.what()) == "no_values: group a has no stored values"); CHECK(err.code == "no_values");
}
TEST_CASE("only the listed groups change, and out lists them in request order") {
  FakeMemory m; three(m); Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(100)), G("b", I32, 4, 0, eq(100)), G("c", I32, 4, 0, eq(100))}, false, s);
  m.poke<int32_t>(0x10000, 90);
  CHECK(e.narrow({{"c", {Pred::LtPrev}}, {"a", eq(77)}}, true, g, s));  // one match in total is enough
  REQUIRE(g.size() == 2); CHECK(g[0].id == "c"); CHECK(g[0].count == 1); CHECK(g[1].id == "a"); CHECK(g[1].count == 0);
  CHECK(e.groups()[1].count == 3);
  m.poke<int32_t>(0x10000, 100); m.poke<int32_t>(0x10010, 80);          // b still holds the scan's values
  CHECK(e.narrow({{"b", {Pred::LtPrev}}}, true, g, s)); REQUIRE(g[0].count == 1);
  CHECK(e.peek("b", 0, 9)[0].addr == 0x10010);
}
TEST_CASE("a refused narrow changes nothing") {
  FakeMemory m; three(m); Engine e(m, {.budget_bytes = 1000}); ScanStats s; std::vector<GroupInfo> g;
  auto sc = e.scan({G("a", I32, 4, 0, eq(100)), G("z", U8, 1, 0, eq(0))}, false, s);   // z's values do not fit
  REQUIRE(sc[0].tracked); REQUIRE_FALSE(sc[1].tracked);
  m.poke<int32_t>(0x10000, 85);
  CHECK_THROWS_WITH_AS(e.narrow({{"a", eq(85)}, {"z", eq(0)}}, true, g, s),          // z is untracked
                       doctest::Contains("no_such_group"), EngineError);
  CHECK_THROWS_WITH_AS(e.narrow({{"a", eq(85)}, {"nope", eq(0)}}, true, g, s),
                       doctest::Contains("no_such_group"), EngineError);
  CHECK_THROWS_WITH_AS(e.narrow({{"a", eq(85)}, {"a", eq(85)}}, true, g, s),
                       doctest::Contains("bad_request"), EngineError);
  CHECK(e.groups()[0].count == 3);
}
TEST_CASE("a narrow that is not applied reports nothing lost and the groups as they stand") {
  FakeMemory m; three(m); m.add_region(0x50000, 4096); m.poke<int32_t>(0x50000, 100);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.remove_region(0x50000);
  CHECK_FALSE(e.narrow({{"a", eq(77)}}, true, g, s));
  CHECK(s.lost == 0); CHECK(s.bytes == 36); REQUIRE(g.size() == 1); CHECK(g[0].count == 4);
}
TEST_CASE("runs end at block boundaries, and values follow positions across blocks") {
  FakeMemory m; m.add_region(0x10000, 3 * 4096);
  for (int i = 0; i < 3; i++) m.poke<int32_t>(0x10000 + i * 4096, 100);
  Engine e(m, {.block_size = 4096}); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.poke<int32_t>(0x10000, 50); m.poke<int32_t>(0x11000, 150); m.poke<int32_t>(0x12000, 70);
  m.read_calls = 0;
  CHECK(e.narrow({{"a", {Pred::LtPrev}}}, true, g, s)); CHECK(m.read_calls == 3); CHECK(s.bytes == 12);
  CHECK(g[0].count == 2);
  m.poke<int32_t>(0x12000, 60);                                         // 50 stays put, 60 is below 70
  CHECK(e.narrow({{"a", {Pred::LtPrev}}}, true, g, s)); REQUIRE(g[0].count == 1);
  CHECK(e.peek("a", 0, 9)[0].addr == 0x12000);
}
TEST_CASE("a short read keeps what arrived and loses the rest") {
  FakeMemory m; three(m); Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.remove_region(0x10000); m.add_region(0x10000, 0x18);                // freed, and a smaller region mapped there
  m.poke<int32_t>(0x10000, 100); m.poke<int32_t>(0x10010, 100);
  CHECK(e.narrow({{"a", eq(100)}}, true, g, s)); CHECK(g[0].count == 2); CHECK(s.lost == 1); CHECK(s.bytes == 0x18);
}
TEST_CASE("a freed page in a dense run loses only the candidates in it") {
  // 256 candidates 16 KiB apart make one run over the whole 4 MiB; reads get all or nothing
  FakeMemory m; m.set_all_or_nothing(true); const uint64_t base = 0x10000000; m.add_region(base, 4u << 20);
  for (uint64_t i = 0; i < 256; i++) m.poke<int32_t>(base + i * 16384 + 64, 100);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  REQUIRE(e.groups()[0].count == 256);
  const uint64_t run = 255 * 16384 + 4;
  m.read_calls = 0;
  CHECK(e.narrow({{"a", eq(100)}}, true, g, s)); CHECK(m.read_calls == 1); CHECK(s.bytes == run);
  m.free_page(base + 5 * 16384 + 4096);                                 // holds no candidate
  uint8_t probe[8]; CHECK(m.read(base + 5 * 16384 + 4096 - 4, probe, 8) == 0);
  m.read_calls = 0;
  CHECK(e.narrow({{"a", eq(100)}}, true, g, s)); CHECK(s.lost == 0); CHECK(g[0].count == 256);
  CHECK(s.bytes == run - 4096); CHECK(m.read_calls < 64);               // split, not page by page
  m.free_page(base + 9 * 16384);                                        // holds the tenth candidate
  CHECK(e.narrow({{"a", eq(100)}}, true, g, s)); CHECK(s.lost == 1); CHECK(g[0].count == 255);
  CHECK(s.bytes == run - 8192);
  auto p = e.peek("a", 9, 1); REQUIRE(p.size() == 1); CHECK(p[0].addr == base + 10 * 16384 + 64);
}
TEST_CASE("a candidate whose bytes run into a freed page is lost") {
  FakeMemory m; m.set_all_or_nothing(true); m.add_region(0x10000, 3 * 4096);
  m.poke<int32_t>(0x10ffe, 100); m.poke<int32_t>(0x10100, 100); m.poke<int32_t>(0x12000, 100);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g; e.scan({G("u", I32, 1, 0, eq(100))}, false, s);
  REQUIRE(e.groups()[0].count == 3);
  m.free_page(0x11000);
  CHECK(e.narrow({{"u", eq(100)}}, true, g, s)); CHECK(s.lost == 1); CHECK(g[0].count == 2);
  CHECK(e.peek("u", 0, 9)[1].addr == 0x12000);
}
TEST_CASE("a region that lost its middle pages keeps the candidates still listed, one read per part") {
  // reads get all or nothing; the run over four pages fails, then the first and last pages, which
  // are still listed, are read with one call each
  FakeMemory m; m.set_all_or_nothing(true); m.add_region(0x10000, 4 * 4096);
  for (int i = 0; i < 4; i++) m.poke<int32_t>(0x10040 + i * 4096, 100);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.remove_region(0x10000); m.add_region(0x10000, 4096); m.add_region(0x13000, 4096);
  m.poke<int32_t>(0x10040, 100); m.poke<int32_t>(0x13040, 100);
  m.read_calls = 0;
  CHECK(e.narrow({{"a", eq(100)}}, true, g, s)); CHECK(g[0].count == 2); CHECK(s.lost == 2);
  CHECK(s.bytes == (4096 - 0x40) + 0x44); CHECK(m.read_calls == 3);
  auto p = e.peek("a", 0, 9); REQUIRE(p.size() == 2); CHECK(p[1].addr == 0x13040);
}
TEST_CASE("a region made read-only after the scan loses only the candidates in its freed page") {
  // reads get all or nothing; the run fails on the freed page, and the read-only pages, no longer
  // writable but still readable, keep their candidates
  FakeMemory m; m.set_all_or_nothing(true); m.add_region(0x10000, 4 * 4096);
  for (int i = 0; i < 4; i++) m.poke<int32_t>(0x10040 + i * 4096, 100);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.set_readonly(0x10000, true); m.free_page(0x11000);
  CHECK(e.narrow({{"a", eq(100)}}, true, g, s)); CHECK(g[0].count == 3); CHECK(s.lost == 1);
  auto p = e.peek("a", 0, 9); REQUIRE(p.size() == 3); CHECK(p[1].addr == 0x12040);
}
TEST_CASE("the game closing in the middle of a narrow changes nothing") {
  DyingMemory m; three(m); m.add_region(0x50000, 4096); m.poke<int32_t>(0x50000, 100);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.reads_left = 1;                                                     // the first run is read, then the game is gone
  CHECK_THROWS_WITH_AS(e.narrow({{"a", eq(100)}}, false, g, s), doctest::Contains("target_gone"), EngineError);
  m.reads_left = 1 << 30; m.set_alive(true);
  CHECK(e.groups()[0].count == 4); CHECK(e.peek("a", 0, 9).size() == 4);
}
TEST_CASE("gained values that would pass the budget are not kept") {
  FakeMemory m; m.add_region(0x10000, 4096);                            // 1024 aligned i32 zeros
  Engine e(m, {.budget_bytes = 1000, .value_cap = 600}); ScanStats s; std::vector<GroupInfo> g;
  auto sc = e.scan({G("a", I32, 4, 0, eq(0))}, false, s);
  REQUIRE(sc[0].tracked); REQUIRE_FALSE(sc[0].has_values);
  for (int i = 500; i < 1024; i++) m.poke<int32_t>(0x10000 + i * 4, 1);
  CHECK(e.narrow({{"a", eq(0)}}, true, g, s));                          // 500 left: 2000 bytes of values
  CHECK(g[0].count == 500); CHECK(g[0].tracked); CHECK_FALSE(g[0].has_values); CHECK(e.storage_bytes() <= 1000);
  CHECK_THROWS_WITH_AS(e.narrow({{"a", {Pred::LtPrev}}}, true, g, s), doctest::Contains("no_values"), EngineError);
}
TEST_CASE("read and write touch only the value's width, and report a closed game") {
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<uint32_t>(0x10000, 0x11223344);
  Engine e(m); uint64_t prev = 0, cur = 0;
  CHECK(e.write_value(0x10000, U8, 0x1AB, prev)); CHECK(prev == 0x44); CHECK(m.peek<uint32_t>(0x10000) == 0x112233AB);
  CHECK(e.read_value(0x10002, I16, cur)); CHECK(cur == 0x1122);
  CHECK_FALSE(e.read_value(0x20000, I32, cur)); CHECK_FALSE(e.write_value(0x20000, I32, 1, prev));
  m.set_alive(false);
  CHECK_THROWS_WITH_AS(e.read_value(0x10000, I32, cur), doctest::Contains("target_gone"), EngineError);
  CHECK_THROWS_WITH_AS(e.write_value(0x10000, I32, 1, prev), doctest::Contains("target_gone"), EngineError);
}
TEST_CASE("a write becomes the stored value of every group of its width at that address") {
  FakeMemory m; three(m); Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(100)), G("u", U32, 4, 0, eq(100)), G("h", I16, 2, 0, eq(100))}, false, s);
  uint64_t prev = 0;
  REQUIRE(e.write_value(0x10000, I32, 999, prev));
  m.poke<int32_t>(0x10000, 990);                                        // down from what was written
  CHECK(e.narrow({{"a", {Pred::LtPrev}}, {"u", {Pred::LtPrev}}, {"h", {Pred::LtPrev}}}, true, g, s));
  REQUIRE(g.size() == 3);
  CHECK(g[0].count == 1); CHECK(g[1].count == 1); CHECK(e.peek("a", 0, 9)[0].addr == 0x10000);
  CHECK(g[2].count == 0);                                               // 2-byte: still compares with 100
  m.poke<int32_t>(0x10010, 7);                                          // a write elsewhere changes nothing stored
  REQUIRE(e.write_value(0x10010, I32, 7, prev));
  m.poke<int32_t>(0x10000, 980);
  CHECK(e.narrow({{"a", {Pred::LtPrev}}}, true, g, s)); CHECK(g[0].count == 1);
}
TEST_CASE("peek reports a closed game") {
  FakeMemory m; three(m); Engine e(m); ScanStats s; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.set_alive(false);
  CHECK_THROWS_WITH_AS(e.peek("a", 0, 9), doctest::Contains("target_gone"), EngineError);
  CHECK_THROWS_WITH_AS(e.peek("nope", 0, 9), doctest::Contains("no_such_group"), EngineError);
}
TEST_CASE("peek_from pages, and fails as peek does") {
  FakeMemory m; three(m); Engine e(m, {.budget_bytes = 1000}); ScanStats s;
  auto sc = e.scan({G("a", I32, 4, 0, eq(100)), G("z", U8, 1, 0, eq(0))}, false, s);   // z's values do not fit
  REQUIRE(sc[0].tracked); REQUIRE_FALSE(sc[1].tracked);
  auto p = e.peek_from("a", 0x10000, 2); REQUIRE(p.size() == 2); CHECK(p[1].addr == 0x10010);
  CHECK(e.peek_from("a", 0x10011, 1)[0].addr == 0x10020);
  CHECK(e.peek_from("a", 0x10000, 0).empty()); CHECK(e.peek_from("z", 0x10000, 9).empty());   // untracked
  CHECK_THROWS_WITH_AS(e.peek_from("nope", 0x10000, 9), doctest::Contains("no_such_group"), EngineError);
  m.set_alive(false);
  CHECK_THROWS_WITH_AS(e.peek_from("a", 0x10000, 9), doctest::Contains("target_gone"), EngineError);
}
TEST_CASE("drop refuses an unknown id and then removes nothing") {
  FakeMemory m; three(m); Engine e(m); ScanStats s;
  e.scan({G("a", I32, 4, 0, eq(100)), G("b", I32, 4, 0, eq(0))}, false, s);
  CHECK_THROWS_WITH_AS(e.drop({"a", "nope"}), doctest::Contains("no_such_group"), EngineError);
  CHECK(e.groups().size() == 2);
  e.drop({"b"}); REQUIRE(e.groups().size() == 1); CHECK(e.groups()[0].id == "a");
}
