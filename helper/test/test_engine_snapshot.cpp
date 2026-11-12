#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "engine/engine.hpp"
#include "test/fake_memory.hpp"
#include "test/test_util.hpp"

using namespace moth;
using namespace moth::test;
using enum ValueType;

static void three(FakeMemory& m) { m.add_region(0x10000, 4096); for (int i = 0; i < 3; i++) m.poke<int32_t>(0x10000 + i * 16, 100); }
TEST_CASE("a snapshot hunt narrows by change alone") {
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<int32_t>(0x10010, 100); m.poke<int32_t>(0x10020, 7);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  auto info = e.snapshot({{"i32", I32, 4}}, s);
  CHECK(info[0].count == 1024); CHECK(info[0].tracked); CHECK(info[0].has_values); CHECK(e.snapshot_bytes() == 4096);
  m.poke<int32_t>(0x10010, 90); m.poke<int32_t>(0x10020, 3);
  CHECK(e.narrow({{"i32", {Pred::LtPrev}}}, true, g, s)); CHECK(g[0].count == 2);
  CHECK(e.snapshot_bytes() == 0);                       // two places keep their own values now
  m.poke<int32_t>(0x10010, 95);                         // up from 90, though still under the 100 of the snapshot
  CHECK(e.narrow({{"i32", {Pred::GtPrev}}}, true, g, s)); CHECK(g[0].count == 1); CHECK(e.peek("i32", 0, 9)[0].addr == 0x10010);
}
TEST_CASE("same first: the snapshot moves on with each step") {
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<int32_t>(0x10010, 100);
  Engine e(m, {.value_cap = 4}); ScanStats s; std::vector<GroupInfo> g;
  e.snapshot({{"i32", I32, 4}}, s);
  m.poke<int32_t>(0x10020, 5);                                              // something else changes
  CHECK(e.narrow({{"i32", {Pred::EqPrev}}}, true, g, s)); CHECK(g[0].count == 1023); CHECK(g[0].has_values);
  CHECK(e.snapshot_bytes() == 4096);                                        // 1023 places still lean on it
  m.poke<int32_t>(0x10010, 90);
  CHECK(e.narrow({{"i32", {Pred::LtPrev}}}, true, g, s)); CHECK(g[0].count == 1); CHECK(e.snapshot_bytes() == 0);
}
TEST_CASE("a snapshot over the limit is refused and the hunt stays") {
  FakeMemory m; three(m); Engine e(m, {.snapshot_cap_bytes = 1024}); ScanStats s;
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  CHECK_THROWS_WITH_AS(e.snapshot({{"i32", I32, 4}}, s), doctest::Contains("too_big"), EngineError);
  CHECK(e.groups().size() == 1); CHECK(e.groups()[0].count == 3);
}

TEST_CASE("two groups of different types from one snapshot each answer the same change their own way") {
  // x: -1.0f to -2.0f, which as an i32 goes up; y: 0 to -1, which as an f32 is not a number
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<float>(0x10010, -1.0f);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.snapshot({{"i32", I32, 4}, {"f32", F32, 4}}, s);
  m.poke<float>(0x10010, -2.0f); m.poke<int32_t>(0x10020, -1);
  CHECK(e.narrow({{"i32", {Pred::LtPrev}}, {"f32", {Pred::LtPrev}}}, true, g, s));
  REQUIRE(g.size() == 2); CHECK(g[0].count == 1); CHECK(g[1].count == 1);
  CHECK(e.peek("i32", 0, 9)[0].addr == 0x10020); CHECK(e.peek("f32", 0, 9)[0].addr == 0x10010);
}
TEST_CASE("a region freed after the snapshot is lost at the next step, and one added after it adds nothing") {
  FakeMemory m; m.add_region(0x10000, 4096); m.add_region(0x50000, 4096);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  CHECK(e.snapshot({{"i32", I32, 4}}, s)[0].count == 2048);
  m.remove_region(0x50000); m.add_region(0x90000, 4096);
  CHECK(e.narrow({{"i32", {Pred::EqPrev}}}, true, g, s)); CHECK(g[0].count == 1024); CHECK(s.lost == 1024);
  CHECK(e.peek_from("i32", 0x50000, 9).empty());
}
TEST_CASE("a region that cannot be read when the snapshot is taken is left out and counted") {
  FakeMemory m; m.add_region(0x10000, 4096); m.add_region(0x50000, 8192); m.set_unreadable(0x50000, true);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  CHECK(e.snapshot({{"i32", I32, 4}}, s)[0].count == 1024);
  CHECK(s.regions == 2); CHECK(s.bytes == 4096); CHECK(s.unreadable_bytes == 8192); CHECK(s.lost == 0);
  CHECK(e.snapshot_bytes() == 4096); CHECK(e.held_snapshot().at(0x50000, 4) == nullptr);
  m.set_unreadable(0x50000, false);                                     // readable now, still no candidate
  CHECK(e.narrow({{"i32", {Pred::EqPrev}}}, true, g, s)); CHECK(g[0].count == 1024); CHECK(s.lost == 0);
  CHECK(e.peek_from("i32", 0x50000, 9).empty());
}
TEST_CASE("a number step on a fresh snapshot leaves the one place") {
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<int32_t>(0x10010, 100);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.snapshot({{"i32", I32, 4}}, s);
  CHECK(e.narrow({{"i32", eq(100)}}, true, g, s)); CHECK(g[0].count == 1); CHECK(g[0].has_values);
  CHECK(e.peek("i32", 0, 9)[0].addr == 0x10010);
  m.poke<int32_t>(0x10010, 97); CHECK(e.narrow({{"i32", {Pred::LtPrev}}}, true, g, s)); CHECK(g[0].count == 1);
}
TEST_CASE("an atomic step that would leave nothing changes nothing, the snapshot's bytes included") {
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<int32_t>(0x10010, 100);
  Engine e(m, {.value_cap = 4}); ScanStats s; std::vector<GroupInfo> g;
  e.snapshot({{"i32", I32, 4}, {"u8", U8, 1}}, s);
  const std::vector<uint8_t> before(e.held_snapshot().at(0x10000, 4096), e.held_snapshot().at(0x10000, 4096) + 4096);
  m.poke<int32_t>(0x10010, 90);                                         // down; nothing goes up
  CHECK_FALSE(e.narrow({{"i32", {Pred::GtPrev}}, {"u8", {Pred::GtPrev}}}, true, g, s));
  CHECK(g[0].count == 1024); CHECK(g[1].count == 4096); CHECK(s.lost == 0); CHECK(e.snapshot_bytes() == 4096);
  REQUIRE(e.held_snapshot().at(0x10000, 4096) != nullptr);
  CHECK(std::memcmp(e.held_snapshot().at(0x10000, 4096), before.data(), 4096) == 0);
  CHECK(e.narrow({{"i32", {Pred::LtPrev}}, {"u8", {Pred::LtPrev}}}, true, g, s));   // still against the snapshot's 100
  CHECK(g[0].count == 1); CHECK(g[1].count == 1); CHECK(e.peek("i32", 0, 9)[0].addr == 0x10010);
}
TEST_CASE("a write to a place that leans on the snapshot is what the next relation compares with") {
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<int32_t>(0x10010, 100);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g; uint64_t prev = 0;
  e.snapshot({{"i32", I32, 4}}, s);
  REQUIRE(e.write_value(0x10010, I32, 200, prev)); CHECK(prev == 100);
  int32_t held = 0; std::memcpy(&held, e.held_snapshot().at(0x10010, 4), 4); CHECK(held == 200);
  m.poke<int32_t>(0x10010, 150);                                        // the game lowers it, still above the 100 it held
  CHECK(e.narrow({{"i32", {Pred::LtPrev}}}, true, g, s)); CHECK(g[0].count == 1); CHECK(e.peek("i32", 0, 9)[0].addr == 0x10010);
}

namespace {

// FakeMemory that notes, at every read, the most snapshot bytes the engine held.
struct WatchingMemory : FakeMemory {
  const Engine* e = nullptr;
  uint64_t most = 0;
  size_t read(uint64_t addr, void* out, size_t len) override {
    if (e != nullptr) most = std::max(most, e->snapshot_bytes());
    return FakeMemory::read(addr, out, len);
  }
};

}  // namespace

TEST_CASE("a second snapshot never holds both, and a new hunt releases the snapshot before it reads") {
  WatchingMemory m; m.add_region(0x10000, 4096); m.add_region(0x50000, 4096);
  Engine e(m); m.e = &e; ScanStats s;
  e.snapshot({{"i32", I32, 4}}, s); CHECK(e.snapshot_bytes() == 8192); CHECK(m.most == 8192);   // both regions, while reading the second
  m.most = 0;
  e.snapshot({{"u8", U8, 1}}, s); CHECK(e.snapshot_bytes() == 8192); CHECK(m.most == 8192);
  REQUIRE(e.groups().size() == 1); CHECK(e.groups()[0].id == "u8");
  m.most = 0;
  e.scan({G("a", I32, 4, 0, eq(0))}, false, s); CHECK(m.most == 0); CHECK(e.snapshot_bytes() == 0);
}

// Beyond the brief: the budget, the workers, and when the snapshot is released.

TEST_CASE("snapshot groups over the budget are untracked from the last one backwards, and the others still narrow") {
  // 1024 i32, 2048 i16 and 4096 u8 places: bitmaps of 128, 256 and 512 bytes
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<int32_t>(0x10010, 100);
  Engine e(m, {.budget_bytes = 600}); ScanStats s; std::vector<GroupInfo> g;
  auto info = e.snapshot({{"i32", I32, 4}, {"i16", I16, 2}, {"u8", U8, 1}}, s);
  CHECK(info[0].tracked); CHECK(info[1].tracked); CHECK_FALSE(info[2].tracked);
  CHECK(info[0].has_values); CHECK(info[1].has_values); CHECK_FALSE(info[2].has_values); CHECK(info[2].count == 4096);
  CHECK(e.storage_bytes() <= 600); CHECK(e.snapshot_bytes() == 4096);
  m.poke<int32_t>(0x10010, 90);
  CHECK(e.narrow({{"i32", {Pred::LtPrev}}, {"i16", {Pred::LtPrev}}}, true, g, s));
  CHECK(g[0].count == 1); CHECK(g[1].count == 1);                      // 100 to 90 is in the low half
  CHECK_THROWS_WITH_AS(e.narrow({{"u8", {Pred::LtPrev}}}, true, g, s), doctest::Contains("no_such_group"), EngineError);
}
TEST_CASE("a snapshot group's align must be a multiple of its width, and a refused snapshot changes nothing") {
  FakeMemory m; three(m); m.read_calls = 0;
  Engine e(m, {.value_cap = 2}); ScanStats s;
  e.snapshot({{"i32", I32, 4}}, s);
  const size_t reads = m.read_calls;
  CHECK_THROWS_WITH_AS(e.snapshot({{"q", I64, 4}}, s), doctest::Contains("bad_request"), EngineError);
  CHECK_THROWS_WITH_AS(e.snapshot({{"b", U8, 0}}, s), doctest::Contains("bad_request"), EngineError);
  CHECK_THROWS_WITH_AS(e.snapshot({{"a", I32, 4}, {"a", F32, 4}}, s), doctest::Contains("bad_request"), EngineError);
  e.set_snapshot_cap_bytes(4095);
  CHECK_THROWS_WITH_AS(e.snapshot({{"b", U8, 1}}, s), "too_big: 4096 to snapshot, limit 4095", EngineError);
  CHECK(m.read_calls == reads);                                         // refused before anything was read
  REQUIRE(e.groups().size() == 1); CHECK(e.groups()[0].id == "i32"); CHECK(e.groups()[0].has_values);
  CHECK(e.snapshot_bytes() == 4096);
  e.set_snapshot_cap_bytes(4096);
  CHECK(e.snapshot({{"h", I16, 6}}, s)[0].count == 683);                // a multiple of the width is enough
}

namespace {

// FakeMemory that, when huge, lists one more writable region after the real ones, too big to copy.
struct HugeMemory : FakeMemory {
  bool huge = false;
  std::vector<Region> writable_regions() override {
    std::vector<Region> out = FakeMemory::writable_regions();
    if (huge) out.push_back({uint64_t{1} << 61, uint64_t{1} << 60});
    return out;
  }
};

// FakeMemory whose read throws once reads_left reaches zero.
struct ThrowingMemory : FakeMemory {
  int reads_left = 1 << 30;
  size_t read(uint64_t addr, void* out, size_t len) override {
    if (reads_left-- <= 0) throw std::runtime_error("read failed");
    return FakeMemory::read(addr, out, len);
  }
};

// FakeMemory whose game closes when a read finds reads_left at zero.
struct DyingMemory : FakeMemory {
  int reads_left = 1 << 30;
  size_t read(uint64_t addr, void* out, size_t len) override {
    if (reads_left-- <= 0) set_alive(false);
    return FakeMemory::read(addr, out, len);
  }
};

}  // namespace

TEST_CASE("a snapshot that cannot get its memory half way releases everything and says so") {
  HugeMemory m; m.add_region(0x10000, 4096);
  Engine e(m, {.snapshot_cap_bytes = uint64_t{1} << 62}); ScanStats s;
  e.scan({G("a", I32, 4, 0, eq(0))}, false, s);
  m.huge = true; m.read_calls = 0;
  CHECK_THROWS_WITH_AS(e.snapshot({{"i32", I32, 4}}, s), "too_big: could not get memory for the snapshot", EngineError);
  CHECK(m.read_calls > 0);                                              // the first region was read
  CHECK(e.snapshot_bytes() == 0); CHECK(e.groups().empty()); CHECK(e.storage_bytes() == 0);
}
TEST_CASE("the snapshot is released when the last group stops leaning on it") {
  // 60 i32 places change all four bytes, and one goes from 100 to 50: i32 is left with 61 places,
  // under value_cap, and keeps its own values; u8 is left with 241 and still leans on the snapshot
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<int32_t>(0x10010, 100);
  Engine e(m, {.value_cap = 200}); ScanStats s; std::vector<GroupInfo> g;
  e.snapshot({{"i32", I32, 4}, {"u8", U8, 1}}, s);
  for (int i = 0; i < 60; i++) m.poke<uint32_t>(0x10100 + i * 4, 0x01010101);
  m.poke<int32_t>(0x10010, 50);
  CHECK(e.narrow({{"i32", {Pred::NePrev}}, {"u8", {Pred::NePrev}}}, true, g, s));
  CHECK(g[0].count == 61); CHECK(g[1].count == 241); CHECK(g[0].has_values); CHECK(g[1].has_values);
  CHECK(e.snapshot_bytes() == 4096);
  m.poke<int32_t>(0x10010, 70);
  CHECK(e.narrow({{"u8", {Pred::GtPrev}}}, true, g, s)); CHECK(g[0].count == 1); CHECK(e.snapshot_bytes() == 0);
  m.poke<int32_t>(0x10010, 60);                                         // under 70, over the 50 i32 kept itself
  CHECK(e.narrow({{"i32", {Pred::GtPrev}}}, true, g, s)); CHECK(g[0].count == 1);
}
TEST_CASE("a group that keeps its own values stops reading the snapshot another group still leans on") {
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<int32_t>(0x10010, 100);
  Engine e(m, {.value_cap = 200}); ScanStats s; std::vector<GroupInfo> g;
  e.snapshot({{"i32", I32, 4}, {"u8", U8, 1}}, s);
  for (int i = 0; i < 60; i++) m.poke<uint32_t>(0x10100 + i * 4, 0x01010101);
  m.poke<int32_t>(0x10010, 50);
  CHECK(e.narrow({{"i32", {Pred::NePrev}}, {"u8", {Pred::NePrev}}}, true, g, s));    // i32 keeps 50 at 0x10010 itself
  for (int i = 0; i < 60; i++) m.poke<uint32_t>(0x10100 + i * 4, 0x03030303);
  m.poke<uint8_t>(0x10010, 40);
  CHECK(e.narrow({{"u8", {Pred::NePrev}}}, true, g, s)); CHECK(g[0].count == 241);   // the snapshot holds 40 there now
  CHECK(e.snapshot_bytes() == 4096);
  m.poke<int32_t>(0x10010, 45);                                         // under the 50 i32 holds, over the snapshot's 40
  CHECK(e.narrow({{"i32", {Pred::LtPrev}}}, true, g, s)); CHECK(g[0].count == 1); CHECK(e.snapshot_bytes() == 4096);
  int32_t held = 0; std::memcpy(&held, e.held_snapshot().at(0x10010, 4), 4); CHECK(held == 40);
}
TEST_CASE("drop, and a keep scan, release the snapshot only with its last group") {
  FakeMemory m; three(m); Engine e(m); ScanStats s;
  e.snapshot({{"i32", I32, 4}, {"u8", U8, 1}, {"i16", I16, 2}}, s);
  e.drop({"u8"}); CHECK(e.snapshot_bytes() == 4096);
  e.scan({G("i16", I16, 2, 0, eq(100))}, true, s); CHECK(e.snapshot_bytes() == 4096);   // i32 still leans on it
  e.scan({G("a", I32, 4, 0, eq(100))}, true, s); CHECK(e.snapshot_bytes() == 4096);
  e.drop({"i32"}); CHECK(e.snapshot_bytes() == 0); CHECK(e.groups().size() == 2);
  e.snapshot({{"i32", I32, 4}}, s);
  e.scan({G("i32", I32, 4, 0, eq(100))}, true, s); CHECK(e.snapshot_bytes() == 0);      // replaced by a scan
}
TEST_CASE("a narrow that fails after it began replacing snapshot bytes releases the snapshot") {
  for (unsigned w : {1u, 4u}) {
    CAPTURE(w);
    ThrowingMemory m; m.add_region(0x10000000, 4u << 20);              // zeros
    Engine e(m, {.value_cap = 4, .block_size = 1u << 20, .workers = w}); ScanStats s; std::vector<GroupInfo> g;
    e.snapshot({{"i32", I32, 4}, {"u8", U8, 1}}, s);
    m.reads_left = 2;                                                   // two runs are read and compared, then a read throws
    CHECK_THROWS_AS(e.narrow({{"i32", {Pred::EqPrev}}, {"u8", {Pred::EqPrev}}}, true, g, s), std::runtime_error);
    m.reads_left = 1 << 30;
    CHECK(e.snapshot_bytes() == 0);
    REQUIRE(e.groups().size() == 2);
    CHECK(e.groups()[0].count == (1u << 20)); CHECK(e.groups()[1].count == (4u << 20));   // no positions changed
    CHECK(e.groups()[0].tracked); CHECK_FALSE(e.groups()[0].has_values); CHECK_FALSE(e.groups()[1].has_values);
    CHECK_THROWS_WITH_AS(e.narrow({{"i32", {Pred::LtPrev}}}, true, g, s), doctest::Contains("no_values"), EngineError);
    CHECK(e.narrow({{"i32", eq(0)}}, true, g, s)); CHECK(g[0].count == (1u << 20));
  }
}
TEST_CASE("the game closing part way through a narrow over the snapshot releases it") {
  DyingMemory m; m.add_region(0x10000000, 4u << 20);
  Engine e(m, {.value_cap = 4, .block_size = 1u << 20}); ScanStats s; std::vector<GroupInfo> g;
  e.snapshot({{"i32", I32, 4}}, s);
  m.set_alive(false);                                                   // before anything is read: the snapshot stays
  CHECK_THROWS_WITH_AS(e.narrow({{"i32", {Pred::EqPrev}}}, true, g, s), doctest::Contains("target_gone"), EngineError);
  CHECK(e.snapshot_bytes() == (4u << 20)); CHECK(e.groups()[0].has_values);
  m.set_alive(true); m.reads_left = 2;
  CHECK_THROWS_WITH_AS(e.narrow({{"i32", {Pred::EqPrev}}}, true, g, s), doctest::Contains("target_gone"), EngineError);
  m.reads_left = 1 << 30; m.set_alive(true);
  CHECK(e.snapshot_bytes() == 0); CHECK(e.groups()[0].count == (1u << 20)); CHECK_FALSE(e.groups()[0].has_values);
}

namespace {

// Three regions with gaps, read all or nothing, of random bytes: a, c, and d, which is freed after
// the snapshot. b cannot be read when the snapshot is taken and can after; e is added after it.
constexpr uint64_t kA = 0x10000000, kB = 0x10100000, kC = 0x10200000, kD = 0x10400000, kE = 0x10500000;

void regions(FakeMemory& m) {
  m.set_all_or_nothing(true);
  for (auto [base, size] : {std::pair{kA, 512u << 10}, {kB, 256u << 10}, {kC, 768u << 10}, {kD, 256u << 10}}) {
    m.add_region(base, size); fill_xorshift(m, base, size, base);
  }
  m.set_unreadable(kB, true);
}

// A group as place-by-place comparison sees it: each place and the value it held at the last step.
struct Model {
  std::string id;
  ValueType type;
  uint32_t align;
  std::vector<std::pair<uint64_t, uint64_t>> places;
};

const std::vector<SnapSpec> kSix = {{"i32", I32, 4}, {"f32", F32, 4}, {"i16", I16, 2},
                                    {"i64", I64, 8}, {"f64", F64, 8}, {"u8", U8, 1}};

// Every aligned place of the readable regions whose bytes can be read, with its value now.
std::vector<Model> model_snapshot(FakeMemory& m, const std::vector<SnapSpec>& specs = kSix) {
  std::vector<Model> out;
  for (const SnapSpec& sp : specs) {
    out.push_back({sp.id, sp.type, sp.align, {}});
    const size_t w = width_of(sp.type);
    for (const Region& r : m.readable_regions())
      for (uint64_t a = r.base; a + w <= r.base + r.size; a += sp.align) {
        uint64_t v = 0;
        if (m.read(a, &v, w) == w) out.back().places.emplace_back(a, v);
      }
  }
  return out;
}

// Keeps each place whose value now matches; returns the places lost.
uint64_t model_step(FakeMemory& m, std::vector<Model>& ms, const Pred& p) {
  uint64_t lost = 0;
  for (Model& md : ms) {
    std::vector<std::pair<uint64_t, uint64_t>> kept;
    const size_t w = width_of(md.type);
    for (const auto& [a, prev] : md.places) {
      uint64_t v = 0;
      if (m.read(a, &v, w) != w) { lost++; continue; }
      if (matches(p, md.type, v, prev)) kept.emplace_back(a, v);
    }
    md.places = std::move(kept);
  }
  return lost;
}

struct Outcome {
  std::vector<std::tuple<uint64_t, bool, bool>> infos;  // count, tracked, has_values
  uint64_t lost = 0, storage = 0, snapshot = 0;
  std::vector<std::vector<uint64_t>> addrs;
  std::vector<uint8_t> held;                            // the snapshot's bytes, page by page
  bool operator==(const Outcome&) const = default;
};

Outcome outcome(Engine& e, const std::vector<GroupInfo>& g, const ScanStats& s) {
  Outcome o;
  for (const GroupInfo& i : g) o.infos.emplace_back(i.count, i.tracked, i.has_values);
  o.lost = s.lost; o.storage = e.storage_bytes(); o.snapshot = e.snapshot_bytes();
  for (const SnapSpec& sp : kSix) {
    o.addrs.emplace_back();
    for (const Item& it : e.peek(sp.id, 0, 1u << 30, true)) o.addrs.back().push_back(it.addr);
  }
  for (uint64_t base : {kA, kB, kC, kD, kE})
    for (uint64_t page = base; page < base + (1u << 20); page += 4096) {
      const uint8_t* p = e.held_snapshot().at(page, 4096);
      o.held.push_back(p != nullptr);
      if (p != nullptr) o.held.insert(o.held.end(), p, p + 4096);
    }
  return o;
}

}  // namespace

TEST_CASE("a snapshot hunt ends the same on any worker count, and as place-by-place comparison would") {
  // value_cap 30,000: after the second step i32, f32, i64 and f64 keep their own values while
  // i16 and u8 still lean on the snapshot; after the third every group keeps its own.
  auto run = [](unsigned w, bool check) {
    FakeMemory m; regions(m);
    Engine e(m, {.value_cap = 30'000, .block_size = 64u << 10, .workers = w}); ScanStats s; std::vector<GroupInfo> g;
    std::vector<Model> model;
    if (check) model = model_snapshot(m);
    const auto info = e.snapshot(kSix, s);
    CHECK(s.unreadable_bytes == (256u << 10)); CHECK(e.snapshot_bytes() == (1536u << 10));
    for (size_t i = 0; i < info.size(); i++) { CHECK(info[i].has_values); if (check) CHECK(info[i].count == model[i].places.size()); }
    m.set_unreadable(kB, false); m.remove_region(kD); m.add_region(kE, 1u << 20);
    fill_xorshift(m, kE, 1u << 20, 5);
    std::vector<Outcome> out;
    auto step = [&](const Pred& p) {
      std::vector<NarrowItem> items;
      for (const SnapSpec& sp : kSix) items.push_back({sp.id, p});
      CHECK(e.narrow(items, true, g, s));
      if (check) {
        CHECK(s.lost == model_step(m, model, p));
        for (size_t i = 0; i < g.size(); i++) { CHECK(g[i].count == model[i].places.size()); CHECK(g[i].has_values); }
      }
      out.push_back(outcome(e, g, s));
      if (check)
        for (size_t i = 0; i < model.size(); i++) {
          std::vector<uint64_t> want;
          for (const auto& pl : model[i].places) want.push_back(pl.first);
          CHECK(out.back().addrs[i] == want);
        }
    };
    for (uint64_t a = kA; a < kA + (512u << 10); a += 4093 * 4) m.poke<int32_t>(a, 7);
    step({Pred::EqPrev});
    if (check) CHECK(e.snapshot_bytes() == (1536u << 10));
    fill_xorshift(m, kA + (64u << 10), 64u << 10, 11); m.free_page(kC + 0x30000);
    step({Pred::NePrev});
    if (check) {
      CHECK(e.snapshot_bytes() == (1536u << 10));
      CHECK(g[0].count <= 30'000); CHECK(g[3].count <= 30'000); CHECK(g[2].count > 30'000); CHECK(g[5].count > 30'000);
    }
    fill_xorshift(m, kA + (64u << 10), 16u << 10, 13);
    step({Pred::LtPrev});
    if (check) CHECK(e.snapshot_bytes() == 0);
    step({Pred::EqPrev});
    return out;
  };
  const std::vector<Outcome> one = run(1, true);
  REQUIRE(one.size() == 4);
  CHECK(one[0].lost > 0); CHECK(one[1].lost > 0);
  for (unsigned w : {4u, 8u}) {
    CAPTURE(w);
    const std::vector<Outcome> other = run(w, false);
    REQUIRE(other.size() == one.size());
    for (size_t i = 0; i < one.size(); i++) {
      CAPTURE(i);
      CHECK(other[i].infos == one[i].infos); CHECK(other[i].lost == one[i].lost);
      CHECK(other[i].storage == one[i].storage); CHECK(other[i].snapshot == one[i].snapshot);
      CHECK(other[i].addrs == one[i].addrs); CHECK(other[i].held == one[i].held);
    }
  }
}
TEST_CASE("every aligned offset of a block, stored as a bitmap") {
  PositionSet full(4), listed(4);
  full.add_full_block(0x1001, 64);
  std::vector<uint32_t> offsets;
  for (uint32_t o = 3; o < 64; o += 4) offsets.push_back(o);
  listed.add_block(0x1001, 64, offsets);
  REQUIRE(full.blocks().size() == 1); CHECK(full.blocks()[0].bitmap); CHECK(full.count() == 16);
  CHECK(full.addresses(0, 99) == listed.addresses(0, 99));
  full.add_full_block(0x2000, 3);                                       // one place
  CHECK(full.count() == 17); CHECK(full.addresses(16, 9) == std::vector<uint64_t>{0x2000});
  full.add_full_block(0x3001, 3);                                       // none
  CHECK(full.count() == 17); CHECK(full.blocks().size() == 2);
}
TEST_CASE("relations compare with the snapshot and with stored values exactly as matches() does, for every type") {
  // each pair of bit patterns of a width, the first held when the snapshot is taken and the second
  // after: integer edges, and for 4 and 8 bytes float zeros, ones, infinities, NaNs and denormals
  const std::vector<std::vector<uint64_t>> patterns = {
      {},
      {0x00, 0x01, 0x7f, 0x80, 0x81, 0xfe, 0xff, 0x40},
      {0x0000, 0x0001, 0x7fff, 0x8000, 0x8001, 0xfffe, 0xffff, 0x1234},
      {},
      {0, 1, 2, 0x7fffffff, 0x80000000, 0x80000001, 0xffffffff, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000,
       0x7fc00000, 0xffc00001, 0x7f7fffff},
      {}, {}, {},
      {0, 1, 0x7fffffffffffffff, 0x8000000000000000, 0xffffffffffffffff, 0x3ff0000000000000, 0xbff0000000000000,
       0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0xfff8000000000001, 0x7fefffffffffffff}};
  const Pred::Op ops[] = {Pred::LtPrev, Pred::GtPrev, Pred::EqPrev, Pred::NePrev};
  for (ValueType t : {I8, U8, I16, U16, I32, U32, F32, I64, U64, F64}) {
    const size_t w = width_of(t);
    const std::vector<uint64_t>& ps = patterns[w];
    const size_t n = ps.size() * ps.size();
    auto put = [&](FakeMemory& m, bool after) {
      bool written = true;
      for (size_t i = 0; i < n; i++) {
        const uint64_t v = after ? ps[i % ps.size()] : ps[i / ps.size()];
        written = m.write(0x10000 + i * 8, &v, w) && written;
      }
      REQUIRE(written);
    };
    for (Pred::Op op : ops) {
      for (bool stored : {false, true}) {   // stored: an EqPrev step first, which keeps every place but a NaN
        CAPTURE(static_cast<int>(t)); CAPTURE(static_cast<int>(op)); CAPTURE(stored);
        std::vector<uint64_t> want;
        for (size_t i = 0; i < n; i++) {
          const uint64_t before = ps[i / ps.size()], after = ps[i % ps.size()];
          if ((!stored || matches({Pred::EqPrev}, t, before, before)) && matches({op}, t, after, before))
            want.push_back(0x10000 + i * 8);
        }
        FakeMemory m; m.add_region(0x10000, n * 8); put(m, false);
        Engine e(m); ScanStats s; std::vector<GroupInfo> g;
        e.snapshot({{"v", t, 8}}, s);
        if (stored) { REQUIRE(e.narrow({{"v", {Pred::EqPrev}}}, true, g, s)); REQUIRE(e.snapshot_bytes() == 0); }
        put(m, true);
        e.narrow({{"v", {op}}}, false, g, s);
        std::vector<uint64_t> got;
        for (const Item& it : e.peek("v", 0, n)) got.push_back(it.addr);
        CHECK(got == want);
      }
    }
  }
}
TEST_CASE("a snapshot hunt whose blocks are split into runs compares each place with its own bytes") {
  // 4 MiB of random bytes in one block, read all or nothing, with a page that cannot be read when
  // the snapshot is taken. Clusters of changed bytes 96 KiB apart leave each group's block split
  // into runs: a dense cluster is a bitmap part, a lone place a list part, and the run around the
  // page has the snapshot's bytes in two stretches.
  constexpr uint64_t base = 0x10000000, hole = base + 0x200000;
  for (unsigned w : {1u, 4u}) {
    CAPTURE(w);
    FakeMemory m; m.set_all_or_nothing(true); m.add_region(base, 4u << 20); fill_xorshift(m, base, 4u << 20, 21);
    m.free_page(hole);
    Engine e(m, {.value_cap = 10, .workers = w}); ScanStats s; std::vector<GroupInfo> g;
    std::vector<Model> model = model_snapshot(m);
    e.snapshot(kSix, s); CHECK(s.unreadable_bytes == 4096);
    auto change = [&](uint64_t seed, uint64_t every) {
      for (uint64_t at = base + 0x1000, k = 0; at + 256 <= base + (4u << 20); at += 96u << 10, k++) {
        if (k % every != 0) continue;
        fill_xorshift(m, at, k % 2 == 0 ? 256 : 8, seed + k);     // dense, or a few places
      }
      fill_xorshift(m, hole - 64, 64, seed); fill_xorshift(m, hole + 4096, 64, seed + 1);   // either side of the page
    };
    auto step = [&](Pred::Op op) {
      std::vector<NarrowItem> items;
      for (const SnapSpec& sp : kSix) items.push_back({sp.id, {op}});
      CHECK(e.narrow(items, true, g, s));
      CHECK(s.lost == model_step(m, model, {op}));
      for (size_t i = 0; i < g.size(); i++) {
        CAPTURE(i);
        CHECK(g[i].count == model[i].places.size());
        std::vector<uint64_t> want, got;
        for (const auto& pl : model[i].places) want.push_back(pl.first);
        for (const Item& it : e.peek(kSix[i].id, 0, 1u << 30, true)) got.push_back(it.addr);
        CHECK(got == want);
      }
    };
    change(100, 1);
    step(Pred::NePrev); CHECK(g[5].count > 2000); CHECK(e.snapshot_bytes() == (4u << 20));
    change(200, 3);
    step(Pred::LtPrev); CHECK(e.snapshot_bytes() == (4u << 20));
    change(300, 2);
    step(Pred::NePrev);
    step(Pred::EqPrev);
  }
}
