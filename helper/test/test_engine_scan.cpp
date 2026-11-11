#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "engine/engine.hpp"
#include "test/fake_memory.hpp"
#include "test/test_util.hpp"

using namespace moth;
using namespace moth::test;
using enum ValueType;

namespace {

// The code of the EngineError f throws, or "" when it throws none.
template <class F> std::string error_code(F&& f) {
  try {
    f();
  } catch (const EngineError& e) {
    return e.code;
  }
  return "";
}

}  // namespace

TEST_CASE("finds values across regions in address order") {
  FakeMemory m; m.add_region(0x10000, 4096); m.add_region(0x30000, 4096);
  m.poke<int32_t>(0x30010, 100); m.poke<int32_t>(0x10008, 100);
  Engine e(m); ScanStats s; auto g = e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  CHECK(g[0].count == 2); CHECK(g[0].tracked); CHECK(g[0].has_values);
  auto p = e.peek("a", 0, 10); CHECK(p[0].addr == 0x10008); CHECK(p[1].addr == 0x30010); CHECK(p[0].bits == 100);
  CHECK(s.regions == 2); CHECK(s.bytes == 8192);
}
TEST_CASE("value straddling a block boundary is found once") {
  FakeMemory m; m.add_region(0x10000, 8192); m.poke<int32_t>(0x10000 + 4094, 100);
  Engine e(m, {.block_size = 4096}); ScanStats s;
  CHECK(e.scan({G("u", I32, 1, 0, eq(100))}, false, s)[0].count == 1);
}
TEST_CASE("unreadable region is counted, not fatal") {
  FakeMemory m; m.add_region(0x10000, 4096); m.set_unreadable(0x10000, true);
  Engine e(m); ScanStats s; CHECK(e.scan({G("a", I32, 4, 0, eq(100))}, false, s)[0].count == 0);
  CHECK(s.unreadable_bytes == 4096);
}
TEST_CASE("a freed page in a block hides only its own bytes") {
  // reads get all or nothing; the hole is in the middle of one 4 MiB block, or starts a 1 MiB
  // block whose neighbour's read runs 7 bytes into it
  for (size_t block : {size_t{16} << 20, size_t{1} << 20}) {
    CAPTURE(block);
    FakeMemory m; m.set_all_or_nothing(true); const uint64_t base = 0x10000000; m.add_region(base, 4u << 20);
    const uint64_t hole = base + (2u << 20);
    for (uint64_t a : {base + 8, hole - 8, hole + 64, hole + 4096, base + (4u << 20) - 4}) m.poke<int32_t>(a, 100);
    m.poke<int32_t>(base + 0x101, 100);                                 // unaligned, readable
    m.poke<uint16_t>(hole - 2, 100);                                    // an unaligned 100 only with the hole's zeros
    m.free_page(hole);
    Engine e(m, {.block_size = block}); ScanStats s;
    auto g = e.scan({G("a", I32, 4, 0, eq(100)), G("u", I32, 1, 4, eq(100))}, false, s);
    CHECK(g[0].count == 4); CHECK(g[1].count == 1);
    CHECK(s.unreadable_bytes == 4096); CHECK(s.bytes == (4u << 20) - 4096);
    auto p = e.peek("a", 0, 9);
    REQUIRE(p.size() == 4); CHECK(p[1].addr == hole - 8); CHECK(p[2].addr == hole + 4096);
    CHECK(e.peek("u", 0, 9)[0].addr == base + 0x101);
  }
}
TEST_CASE("a readable block is read with one call") {
  FakeMemory m; m.set_all_or_nothing(true); m.add_region(0x10000000, 4u << 20);
  Engine e(m); ScanStats s; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  CHECK(m.read_calls == 1); CHECK(s.unreadable_bytes == 0);
}
TEST_CASE("values are kept only up to value_cap") {
  FakeMemory m; m.add_region(0x10000, 4096); for (int i = 0; i < 5; i++) m.poke<int32_t>(0x10000 + i * 4, 100);
  Engine e(m, {.value_cap = 4}); ScanStats s; auto g = e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  CHECK(g[0].count == 5); CHECK(g[0].tracked); CHECK_FALSE(g[0].has_values);
}
TEST_CASE("dense group is stored as a bitmap") {
  FakeMemory m; m.add_region(0x100000, 1 << 20);                       // 1 MiB of zeros
  Engine e(m, {.value_cap = 0}); ScanStats s; auto g = e.scan({G("z", U8, 1, 0, eq(0))}, false, s);
  CHECK(g[0].count == (1u << 20)); CHECK(e.storage_bytes() <= 140'000);
}
TEST_CASE("over budget, the last groups become untracked") {
  FakeMemory m; m.add_region(0x100000, 1 << 20);
  Engine e(m, {.budget_bytes = 150'000, .value_cap = 0}); ScanStats s;
  auto g = e.scan({G("z", U8, 1, 0, eq(0)), G("y", I16, 2, 0, eq(0))}, false, s);
  CHECK(g[0].tracked); CHECK_FALSE(g[1].tracked); CHECK(g[1].count == (1u << 19));
}
TEST_CASE("keep replaces only the listed groups") {
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<int32_t>(0x10000, 100); m.poke<int32_t>(0x10004, 7);
  Engine e(m); ScanStats s; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  e.scan({G("b", I32, 4, 0, eq(7))}, true, s); CHECK(e.groups().size() == 2);
  e.scan({G("c", I32, 4, 0, eq(7))}, false, s); CHECK(e.groups().size() == 1);
}

// Beyond the brief: the storage itself, and the choices the brief leaves to the engine.

TEST_CASE("a block is stored as offsets or as a bitmap, whichever is smaller") {
  std::vector<uint32_t> few = {8, 4000}, many;
  for (uint32_t o = 0; o < 4096; o += 8) many.push_back(o);           // 512 of 1024 aligned slots
  PositionSet sparse(4), dense(4);
  sparse.add_block(0x10000, 4096, few); dense.add_block(0x10000, 4096, many);
  const uint64_t block = sizeof(PositionSet::Block);
  CHECK_FALSE(sparse.blocks()[0].bitmap); CHECK(sparse.bytes() == block + 2 * 4);      // two offsets
  CHECK(dense.blocks()[0].bitmap); CHECK(dense.count() == 512); CHECK(dense.bytes() == block + 1024 / 8);  // 1024 slots
  CHECK(dense.addresses(0, 3) == std::vector<uint64_t>{0x10000, 0x10008, 0x10010});
  CHECK(sparse.addresses(1, 5) == std::vector<uint64_t>{0x10000 + 4000});
  PositionSet none(4); none.add_block(0x10000, 4096, {}); CHECK(none.blocks().empty()); CHECK(none.bytes() == 0);
}
TEST_CASE("bitmap slots start at the first aligned address of the block") {
  // base 0x1000e is 2 past a multiple of 4, so offset 2 is the first aligned slot
  std::vector<uint32_t> all;
  for (uint32_t o = 2; o < 1000; o += 4) all.push_back(o);
  PositionSet s(4); s.add_block(0x1000e, 1000, all);
  REQUIRE(s.blocks()[0].bitmap);
  CHECK(s.addresses(0, 2) == std::vector<uint64_t>{0x10010, 0x10014});
  CHECK(s.addresses(249, 9) == std::vector<uint64_t>{0x1000e + 998});
  std::vector<uint64_t> seen; s.for_each([&](uint64_t a) { seen.push_back(a); });
  REQUIRE(seen.size() == 250); CHECK(seen.front() == 0x10010); CHECK(seen.back() == 0x1000e + 998);
}
TEST_CASE("a position's index is found in offset and bitmap blocks") {
  std::vector<uint32_t> many;
  for (uint32_t o = 0; o < 4096; o += 8) many.push_back(o);           // a bitmap
  PositionSet s(4);
  s.add_block(0x10000, 4096, std::vector<uint32_t>{8, 4000});           // offsets
  s.add_block(0x11000, 4096, many);
  REQUIRE(s.blocks()[1].bitmap);
  uint64_t i = 99;
  CHECK(s.index_of(0x10008, i)); CHECK(i == 0);
  CHECK(s.index_of(0x10000 + 4000, i)); CHECK(i == 1);
  CHECK(s.index_of(0x11000, i)); CHECK(i == 2);
  CHECK(s.index_of(0x11000 + 8 * 300, i)); CHECK(i == 302);
  CHECK(s.index_of(0x11000 + 4088, i)); CHECK(i == 2 + 511);
  CHECK_FALSE(s.index_of(0x1000c, i)); CHECK_FALSE(s.index_of(0x11004, i)); CHECK_FALSE(s.index_of(0x11002, i));
  CHECK_FALSE(s.index_of(0xf000, i)); CHECK_FALSE(s.index_of(0x12000, i));
}
TEST_CASE("peek pages by index across blocks and reads current values") {
  FakeMemory m; m.add_region(0x10000, 3 * 4096); m.poke<uint8_t>(0x11000, 9);   // second block's first byte
  Engine e(m, {.block_size = 4096}); ScanStats s; e.scan({G("z", U8, 1, 0, eq(0))}, false, s);
  CHECK(e.groups()[0].count == 3 * 4096 - 1);
  auto p = e.peek("z", 4094, 3);
  REQUIRE(p.size() == 3);
  CHECK(p[0].addr == 0x10ffe); CHECK(p[1].addr == 0x10fff); CHECK(p[2].addr == 0x11001);
  m.poke<uint8_t>(0x10fff, 42); CHECK(e.peek("z", 4095, 1)[0].bits == 42);
  CHECK(e.peek("z", 3 * 4096 - 2, 10).size() == 1);
  CHECK(error_code([&] { e.peek("nope", 0, 1); }) == "no_such_group");
}
TEST_CASE("peek leaves out positions that can no longer be read") {
  FakeMemory m; m.add_region(0x10000, 4096); m.add_region(0x30000, 4096);
  m.poke<int32_t>(0x10000, 100); m.poke<int32_t>(0x30000, 100);
  Engine e(m); ScanStats s; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.remove_region(0x10000);
  auto p = e.peek("a", 0, 10); REQUIRE(p.size() == 1); CHECK(p[0].addr == 0x30000);
}
TEST_CASE("stats split readable and unreadable bytes") {
  FakeMemory m; m.add_region(0x10000, 4096); m.add_region(0x30000, 8192); m.set_unreadable(0x30000, true);
  Engine e(m); ScanStats s; e.scan({G("a", I32, 4, 0, eq(0))}, false, s);
  CHECK(s.regions == 2); CHECK(s.bytes == 4096); CHECK(s.unreadable_bytes == 8192); CHECK(s.lost == 0);
}
TEST_CASE("stored values count toward the budget") {
  FakeMemory m; m.add_region(0x10000, 4096);                            // 1024 aligned i32 zeros
  ScanStats s;
  Engine with(m, {.budget_bytes = 4096});                               // values alone are 8 KiB
  auto g = with.scan({G("a", I32, 4, 0, eq(0))}, false, s);
  CHECK_FALSE(g[0].tracked); CHECK_FALSE(g[0].has_values); CHECK(g[0].count == 1024);
  CHECK(with.storage_bytes() == 0); CHECK(with.peek("a", 0, 10).empty());
  Engine without(m, {.budget_bytes = 4096, .value_cap = 0});
  CHECK(without.scan({G("a", I32, 4, 0, eq(0))}, false, s)[0].tracked);
}
TEST_CASE("values dropped later in the scan do not untrack a group early") {
  // block one: 800 zeros, enough values to pass the budget; block two takes the count past
  // value_cap, the values go, and the group fits again
  FakeMemory m; m.add_region(0x10000, 8192);
  for (int i = 800; i < 1024; i++) m.poke<int32_t>(0x10000 + i * 4, 1);
  for (int i = 300; i < 1024; i++) m.poke<int32_t>(0x11000 + i * 4, 1);
  Engine e(m, {.budget_bytes = 5000, .value_cap = 1000, .block_size = 4096}); ScanStats s;
  auto g = e.scan({G("a", I32, 4, 0, eq(0))}, false, s);
  CHECK(g[0].count == 1100); CHECK(g[0].tracked); CHECK_FALSE(g[0].has_values);
}
TEST_CASE("keep rescans a listed group that already exists") {
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<int32_t>(0x10000, 100); m.poke<int32_t>(0x10004, 7);
  Engine e(m); ScanStats s;
  e.scan({G("a", I32, 4, 0, eq(100)), G("b", I32, 4, 0, eq(7))}, false, s);
  auto g = e.scan({G("a", I32, 4, 0, eq(7))}, true, s);
  REQUIRE(e.groups().size() == 2); CHECK(g.size() == 1);
  CHECK(e.peek("a", 0, 10)[0].addr == 0x10004); CHECK(e.peek("b", 0, 10)[0].addr == 0x10004);
}
TEST_CASE("a bad request leaves the groups as they were") {
  FakeMemory m; m.add_region(0x10000, 4096); Engine e(m); ScanStats s;
  e.scan({G("a", I32, 4, 0, eq(0))}, false, s);
  CHECK(error_code([&] { e.scan({G("p", I32, 4, 0, Pred{Pred::LtPrev})}, false, s); }) == "bad_request");
  CHECK(error_code([&] { e.scan({G("b", I32, 4, 0, eq(0)), G("b", I16, 2, 0, eq(0))}, false, s); }) == "bad_request");
  REQUIRE(e.groups().size() == 1); CHECK(e.groups()[0].id == "a"); CHECK(e.groups()[0].count == 1024);
}
TEST_CASE("a block size that offsets cannot address is refused") {
  FakeMemory m;
  CHECK(error_code([&] { Engine e(m, {.block_size = 0}); }) == "bad_request");
  CHECK(error_code([&] { Engine e(m, {.block_size = (size_t{1} << 30) + 1}); }) == "bad_request");
  CHECK(error_code([&] { Engine e(m, {.block_size = size_t{1} << 30}); }) == "");
}
TEST_CASE("scanning a closed target is reported") {
  FakeMemory m; m.add_region(0x10000, 4096); Engine e(m); ScanStats s;
  m.set_alive(false);
  CHECK(error_code([&] { e.scan({G("a", I32, 4, 0, eq(0))}, false, s); }) == "target_gone");
}
