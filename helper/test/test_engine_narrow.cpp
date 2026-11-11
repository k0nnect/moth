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
TEST_CASE("freed memory loses candidates instead of failing") {
  FakeMemory m; three(m); m.add_region(0x50000, 4096); m.poke<int32_t>(0x50000, 100);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.remove_region(0x50000); CHECK(e.narrow({{"a", eq(100)}}, true, g, s)); CHECK(g[0].count == 3); CHECK(s.lost == 1);
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
  CHECK(e.narrow({{"a", eq(0)}}, true, g, s));                          // 500 left: 4000 bytes of values
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
TEST_CASE("drop refuses an unknown id and then removes nothing") {
  FakeMemory m; three(m); Engine e(m); ScanStats s;
  e.scan({G("a", I32, 4, 0, eq(100)), G("b", I32, 4, 0, eq(0))}, false, s);
  CHECK_THROWS_WITH_AS(e.drop({"a", "nope"}), doctest::Contains("no_such_group"), EngineError);
  CHECK(e.groups().size() == 2);
  e.drop({"b"}); REQUIRE(e.groups().size() == 1); CHECK(e.groups()[0].id == "a");
}
