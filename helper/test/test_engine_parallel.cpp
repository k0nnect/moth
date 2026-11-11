#include <algorithm>
#include <stdexcept>
#include <string>
#include <thread>
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

namespace {

// Each group's item addresses, in order.
std::vector<std::vector<uint64_t>> addrs(const std::vector<std::vector<Item>>& groups) {
  std::vector<std::vector<uint64_t>> out;
  for (const auto& items : groups) {
    out.emplace_back();
    for (const Item& it : items) out.back().push_back(it.addr);
  }
  return out;
}

}  // namespace

TEST_CASE("worker count does not change results") {
  FakeMemory m; m.add_region(0x10000000, 64u << 20); fill_xorshift(m, 0x10000000, 64u << 20, 0x6d6f7468);
  std::vector<GroupSpec> gs = {G("b", U8, 1, 0, eq(30)), G("a", I32, 4, 0, eq(100)), G("f", F32, 4, 0, range(99, 101))};
  auto run = [&](unsigned w) { Engine e(m, {.block_size = 1u << 20, .workers = w}); ScanStats s; e.scan(gs, false, s);
    std::vector<std::vector<Item>> out; for (auto& g : gs) out.push_back(e.peek(g.id, 0, 1u << 30)); return out; };
  auto one = run(1), four = run(4), autow = run(0);
  CHECK(addrs(one) == addrs(four)); CHECK(addrs(one) == addrs(autow)); CHECK(one[0].size() > 200'000);
  Engine e4(m, {.workers = 4}); CHECK(e4.workers_used() == 4);
  Engine e0(m, {.workers = 0}); CHECK(e0.workers_used() >= 1); CHECK(e0.workers_used() <= 8);
}

// Beyond the brief: the budget, bounded memory, merged narrow reads, and the reading thread.

namespace {

// FakeMemory that records where, and on which thread, each read happened.
struct WatchedMemory : FakeMemory {
  std::vector<uint64_t> at;
  std::vector<std::thread::id> by;
  size_t read(uint64_t addr, void* out, size_t len) override {
    at.push_back(addr);
    by.push_back(std::this_thread::get_id());
    return FakeMemory::read(addr, out, len);
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

using Outcome = std::tuple<std::vector<std::tuple<std::string, uint64_t, bool, bool>>, uint64_t,
                           std::vector<std::vector<Item>>>;

}  // namespace

TEST_CASE("worker count does not change which groups a budget untracks") {
  // 8 MiB of zeros, then 16 MiB of random bytes: the zero groups fill fast and pass the budget
  // part way through the walk, the others grow slowly
  FakeMemory m; m.add_region(0x10000000, 24u << 20); fill_xorshift(m, 0x10000000 + (8u << 20), 16u << 20, 7);
  const std::vector<GroupSpec> gs = {G("i32", I32, 4, 0, eq(0)), G("r", U8, 1, 0, eq(30)), G("u8", U8, 1, 0, eq(0)),
                                     G("i16", I16, 2, 0, eq(0)), G("f", F32, 4, 0, range(-1, 1))};
  auto run = [&](unsigned w) {
    Engine e(m, {.budget_bytes = 1600u << 10, .value_cap = 100'000, .block_size = 1u << 20, .workers = w});
    ScanStats s; Outcome o;
    for (const GroupInfo& g : e.scan(gs, false, s)) std::get<0>(o).emplace_back(g.id, g.count, g.tracked, g.has_values);
    std::get<1>(o) = e.storage_bytes();
    for (const GroupSpec& g : gs) std::get<2>(o).push_back(e.peek(g.id, 0, 1u << 30));
    return o;
  };
  const Outcome one = run(1);
  const auto& infos = std::get<0>(one);
  REQUIRE(infos.size() == 5);
  CHECK(std::get<2>(infos[0])); CHECK(std::get<2>(infos[1]));                   // i32 and r fit
  CHECK_FALSE(std::get<2>(infos[3])); CHECK_FALSE(std::get<2>(infos[4]));       // i16 and f do not
  CHECK(std::get<1>(one) <= (1600u << 10));
  for (unsigned w : {2u, 4u, 0u}) {
    const Outcome other = run(w);
    CHECK(std::get<0>(other) == std::get<0>(one));
    CHECK(std::get<1>(other) == std::get<1>(one));
    CHECK(addrs(std::get<2>(other)) == addrs(std::get<2>(one)));
  }
}
TEST_CASE("a dense value holds raw offsets for at most one 1 MiB slice per worker") {
  // 32 MiB of zeros in two default 16 MiB blocks: every byte matches u8 0, every fourth i32 0.
  // Unsliced, one block alone would hold 16 Mi + 4 Mi raw offsets.
  FakeMemory m; m.add_region(0x10000000, 32u << 20);
  const std::vector<GroupSpec> gs = {G("u8", U8, 1, 0, eq(0)), G("i32", I32, 4, 0, eq(0))};
  const uint64_t slice = (1u << 20) + (1u << 18);   // a 1 MiB slice's offsets for both groups
  for (unsigned w : {1u, 4u}) {
    Engine e(m, {.value_cap = 0, .workers = w}); ScanStats s;
    auto g = e.scan(gs, false, s);
    CHECK(g[0].count == (32u << 20)); CHECK(g[1].count == (8u << 20));
    CHECK(e.storage_bytes() <= (5u << 20) + 4096);  // stored as bitmaps
    CHECK(e.peak_raw_offsets() > 0);
    CHECK(e.peak_raw_offsets() <= w * slice);
  }
}
TEST_CASE("a narrow reads each span once for every listed group") {
  // i32 100 at three places more than 64 KiB apart; u8 100 matches the same three addresses
  FakeMemory m; m.add_region(0x10000, 1u << 20);
  for (uint32_t o : {0x100u, 0x20000u, 0x40000u}) m.poke<int32_t>(0x10000 + o, 100);
  auto cost = [&](const std::vector<NarrowItem>& items) {
    Engine e(m); ScanStats s; std::vector<GroupInfo> g;
    e.scan({G("a", I32, 4, 0, eq(100)), G("b", U8, 1, 0, eq(100))}, false, s);
    m.read_calls = 0; m.bytes_read = 0;
    CHECK(e.narrow(items, true, g, s));
    for (const GroupInfo& gi : g) CHECK(gi.count == 3);
    return std::pair{m.read_calls, m.bytes_read};
  };
  const auto one = cost({{"a", eq(100)}});
  const auto two = cost({{"a", eq(100)}, {"b", eq(100)}});
  CHECK(one.first == 3); CHECK(one.second == 12);
  CHECK(two == one);
}
TEST_CASE("candidates of every listed group count toward the run gap") {
  // a's two candidates are 96 KiB apart, two runs on their own; b's candidate halfway joins them
  FakeMemory m; m.add_region(0x10000, 1u << 20);
  m.poke<int32_t>(0x10000, 100); m.poke<int32_t>(0x10000 + (96u << 10), 100); m.poke<int16_t>(0x10000 + (48u << 10), 7);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(100)), G("b", I16, 2, 0, eq(7))}, false, s);
  m.read_calls = 0; CHECK(e.narrow({{"a", eq(100)}}, true, g, s)); CHECK(m.read_calls == 2);
  m.read_calls = 0; CHECK(e.narrow({{"b", eq(7)}, {"a", eq(100)}}, true, g, s));
  CHECK(m.read_calls == 1); CHECK(s.bytes == (96u << 10) + 4);
  REQUIRE(g.size() == 2); CHECK(g[0].id == "b"); CHECK(g[0].count == 1); CHECK(g[1].count == 2);
}
TEST_CASE("groups whose blocks do not line up narrow correctly together") {
  // a is scanned in 4 KiB blocks from 0x10000; the region is then remapped from 0xf800, so b's
  // blocks start 2 KiB earlier. Both hold 0x10000 and 0x11800.
  FakeMemory m; m.add_region(0x10000, 8192);
  m.poke<int32_t>(0x10000, 100); m.poke<int32_t>(0x11800, 100);
  Engine e(m, {.block_size = 4096}); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.remove_region(0x10000); m.add_region(0xf800, 3 * 4096);
  m.poke<int32_t>(0x10000, 100); m.poke<int32_t>(0x11800, 100);
  e.scan({G("b", I32, 4, 0, eq(100))}, true, s);
  m.poke<int32_t>(0x10000, 90); m.poke<int32_t>(0x11800, 120);
  CHECK(e.narrow({{"a", {Pred::LtPrev}}, {"b", {Pred::GtPrev}}}, true, g, s));
  REQUIRE(g.size() == 2); CHECK(g[0].count == 1); CHECK(g[1].count == 1); CHECK(s.lost == 0);
  CHECK(e.peek("a", 0, 9)[0].addr == 0x10000); CHECK(e.peek("b", 0, 9)[0].addr == 0x11800);
}
TEST_CASE("blocks are read on the calling thread in address order") {
  WatchedMemory m; m.add_region(0x20000000, 8u << 20); m.add_region(0x10000000, 8u << 20);
  Engine e(m, {.block_size = 1u << 20, .workers = 4}); ScanStats s;
  CHECK(e.scan({G("z", U8, 1, 0, eq(0))}, false, s)[0].count == (16u << 20));
  REQUIRE(m.at.size() == 16);
  CHECK(std::is_sorted(m.at.begin(), m.at.end()));
  CHECK(std::all_of(m.by.begin(), m.by.end(), [](std::thread::id id) { return id == std::this_thread::get_id(); }));
}
TEST_CASE("a read that throws stops the workers and leaves the engine usable") {
  ThrowingMemory m; m.add_region(0x10000000, 16u << 20);
  Engine e(m, {.block_size = 1u << 20, .workers = 4}); ScanStats s;
  m.reads_left = 5;
  CHECK_THROWS_AS(e.scan({G("z", U8, 1, 0, eq(0))}, false, s), std::runtime_error);
  m.reads_left = 1 << 30;
  CHECK(e.scan({G("z", U8, 1, 0, eq(0))}, false, s)[0].count == (16u << 20));
}
