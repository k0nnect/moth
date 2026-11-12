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

namespace {

// Each group's items as (address, value) pairs, in order.
std::vector<std::vector<std::pair<uint64_t, uint64_t>>> rows(const std::vector<std::vector<Item>>& groups) {
  std::vector<std::vector<std::pair<uint64_t, uint64_t>>> out;
  for (const auto& items : groups) {
    out.emplace_back();
    for (const Item& it : items) out.back().emplace_back(it.addr, it.bits);
  }
  return out;
}

constexpr uint64_t kMixed[3][2] = {{0x10000000, 24u << 20}, {0x12000000, 16u << 20}, {0x14000000, 24u << 20}};

// 64 MiB in three regions, read all or nothing: random bytes; in the middle quarter of each region,
// small whole numbers (0 to 7 at every 4-byte place), which are all f32 near 0; and every 3 MiB a
// 96 KiB stretch of 0xff, which no group below matches, so runs also end inside 1 MiB blocks.
void mixed(FakeMemory& m) {
  m.set_all_or_nothing(true);
  std::vector<int32_t> small(1u << 18);
  const std::vector<uint8_t> ff(96u << 10, 0xff);
  for (int r = 0; r < 3; r++) {
    const auto [base, size] = std::pair{kMixed[r][0], kMixed[r][1]};
    m.add_region(base, size); fill_xorshift(m, base, size, 0x6d6f7468 + r);
    for (uint64_t at = base + size / 8 * 3; at < base + size / 8 * 5; at += small.size() * 4) {
      for (size_t i = 0; i < small.size(); i++) small[i] = static_cast<int32_t>(((at + i * 4) * 2654435761u >> 29) & 7);
      REQUIRE(m.write(at, small.data(), small.size() * 4));
    }
    for (uint64_t at = base + 0x48000; at + ff.size() <= base + size; at += 3u << 20) REQUIRE(m.write(at, ff.data(), ff.size()));
  }
}

// Writes v every stride bytes from skip into each region, the middle one only if middle.
void spread(FakeMemory& m, uint64_t stride, uint64_t skip, int32_t v, bool middle) {
  for (int r = 0; r < 3; r++)
    for (uint64_t at = kMixed[r][0] + skip; (middle || r != 1) && at + 4 <= kMixed[r][0] + kMixed[r][1]; at += stride)
      m.poke<int32_t>(at, v);
}

struct Step {
  std::vector<std::tuple<std::string, uint64_t, bool, bool>> infos;
  uint64_t lost = 0, storage = 0;
  std::vector<std::vector<std::pair<uint64_t, uint64_t>>> rows;
  unsigned threads = 0;
};

}  // namespace

TEST_CASE("a narrow leaves the same groups, values and losses on any worker count") {
  const Pred tiny = range(-1e-30, 1e-30);
  const std::vector<GroupSpec> gs = {G("i", I32, 4, 0, eq(3)), G("f", F32, 4, 0, tiny), G("b", U8, 1, 0, eq(0x5a))};
  auto step = [&](Engine& e, const std::vector<NarrowItem>& items) {
    Step st; ScanStats s; std::vector<GroupInfo> g;
    CHECK(e.narrow(items, true, g, s));
    for (const GroupInfo& i : g) st.infos.emplace_back(i.id, i.count, i.tracked, i.has_values);
    st.lost = s.lost; st.storage = e.storage_bytes(); st.threads = e.narrow_threads_used();
    std::vector<std::vector<Item>> items_now;
    for (const GroupSpec& sp : gs) items_now.push_back(e.peek(sp.id, 0, 1u << 30));
    st.rows = rows(items_now);
    return st;
  };
  auto run = [&](unsigned w) {
    FakeMemory m; mixed(m);
    Engine e(m, {.value_cap = 2'000'000, .block_size = 1u << 20, .workers = w}); ScanStats s;
    const auto sc = e.scan(gs, false, s);
    REQUIRE(sc[1].count > 4'000'000); REQUIRE(sc[1].tracked); REQUIRE_FALSE(sc[1].has_values);
    REQUIRE(sc[0].has_values); REQUIRE(sc[2].has_values);                // two groups with values beside one without
    spread(m, 8191 * 4, 0, 5, true); spread(m, 4093 * 4, 2, 0x3f000000, true);   // 5, and bytes off the 4-byte places
    m.remove_region(kMixed[1][0]);                                      // the middle region is freed
    m.free_page(kMixed[0][0] + (9u << 20) + 0x3000);                    // and one page of a run that is read in parts
    Pred moved = range(-10, 10); moved.moved = true;
    std::vector<Step> steps{step(e, {{"i", moved}, {"f", tiny}, {"b", {Pred::EqPrev}}})};
    spread(m, 8191 * 4, 0, 4, false);                                   // down from the 5 just stored
    steps.push_back(step(e, {{"b", {Pred::EqPrev}}, {"i", {Pred::LtPrev}}, {"f", eq(0)}}));   // f gains values
    return steps;
  };
  const std::vector<Step> one = run(1);
  REQUIRE(one.size() == 2);
  CHECK(one[0].lost > 0); CHECK(one[0].threads == 1); CHECK(std::get<3>(one[1].infos[2]));
  for (const Step& st : one) for (const auto& r : st.rows) CHECK(!r.empty());
  for (unsigned w : {2u, 4u, 8u}) {
    const std::vector<Step> other = run(w);
    for (size_t i = 0; i < one.size(); i++) {
      CHECK(other[i].infos == one[i].infos); CHECK(other[i].lost == one[i].lost);
      CHECK(other[i].storage == one[i].storage); CHECK(other[i].rows == one[i].rows);
      CHECK(other[i].threads <= w);
    }
    if (w == 4) CHECK(other[0].threads > 1);
  }
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
TEST_CASE("a run ends inside a bitmap word, and each run's positions compare with their own values") {
  // i32 places every 32 KiB of 2 MiB, each holding its number, held as a bitmap of 64 bits; places
  // 10 to 12 are out of range, so 9 and 13 are 128 KiB apart and the narrow reads two runs. Every
  // value then goes down by one but place 40's, which goes up.
  FakeMemory m; m.add_region(0x100000, 2u << 20);
  auto place = [](uint64_t i) { return 0x100000 + i * 32768; };
  for (unsigned w : {1u, 4u}) {
    for (uint64_t i = 0; i < 64; i++) m.poke<int32_t>(place(i), i >= 10 && i <= 12 ? 1000 : static_cast<int32_t>(i));
    Engine e(m, {.workers = w}); ScanStats s; std::vector<GroupInfo> g;
    REQUIRE(e.scan({G("a", I32, 32768, 0, range(-10, 100))}, false, s)[0].count == 61);
    for (uint64_t i = 0; i < 64; i++) m.poke<int32_t>(place(i), m.peek<int32_t>(place(i)) + (i == 40 ? 1 : -1));
    m.read_calls = 0;
    CHECK(e.narrow({{"a", {Pred::LtPrev}}}, true, g, s)); CHECK(g[0].count == 60);
    CHECK(m.read_calls == 2); CHECK(s.bytes == (9u * 32768 + 4) + (50u * 32768 + 4));
    auto p = e.peek("a", 36, 2); REQUIRE(p.size() == 2); CHECK(p[0].addr == place(39)); CHECK(p[1].addr == place(41));
  }
}
TEST_CASE("each run of a list block compares its positions with their own values") {
  // 10, 20, 30, 40 at 128 KiB steps of one block: four runs; all go down by one but the third
  FakeMemory m; m.add_region(0x100000, 1u << 20);
  auto place = [](uint64_t i) { return 0x100000 + i * (128u << 10); };
  for (unsigned w : {1u, 4u}) {
    for (int i = 0; i < 4; i++) m.poke<int32_t>(place(i), 10 * (i + 1));
    Engine e(m, {.workers = w}); ScanStats s; std::vector<GroupInfo> g;
    REQUIRE(e.scan({G("a", I32, 4, 0, range(5, 45))}, false, s)[0].count == 4);
    for (int i = 0; i < 4; i++) m.poke<int32_t>(place(i), 10 * (i + 1) + (i == 2 ? 1 : -1));
    m.read_calls = 0;
    CHECK(e.narrow({{"a", {Pred::LtPrev}}}, true, g, s)); CHECK(m.read_calls == 4); CHECK(g[0].count == 3);
    auto p = e.peek("a", 0, 9); REQUIRE(p.size() == 3); CHECK(p[2].addr == place(3));
  }
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
TEST_CASE("a narrow's runs hold a bounded amount beyond their bytes, and copy no values a group cannot keep") {
  // 16 MiB of zeros: u64 0 at every byte but the last 7, far over a value_cap of 100,000. A
  // narrow keeps them all, so the group ends without values.
  FakeMemory m; m.add_region(0x10000000, 16u << 20);
  for (unsigned w : {1u, 8u}) {
    Engine e(m, {.value_cap = 100'000, .block_size = 1u << 20, .workers = w}); ScanStats s; std::vector<GroupInfo> g;
    REQUIRE(e.scan({G("q", U64, 1, 0, eq(0))}, false, s)[0].count == (16u << 20) - 7);
    CHECK(e.narrow({{"q", range(-1, 1)}}, true, g, s)); CHECK(g[0].count == (16u << 20) - 7); CHECK_FALSE(g[0].has_values);
    CHECK(e.peak_narrow_held_bytes() > 0);
    CHECK(e.peak_narrow_held_bytes() <= (w + 4) * (1u << 20));
    CHECK(e.peak_narrow_held_bytes() <= 100'000 * 8);          // no more values copied than the cap allows
  }
}
TEST_CASE("values a run has no room for are taken from its bytes, each at its own place") {
  // 2 MiB of random bytes: u64 at every byte, 2,097,145 places, all with values. One 1 MiB run's
  // values take 8 MiB, more than all the room runs may hold, so most are taken from the run's bytes.
  FakeMemory m; m.add_region(0x10000000, 2u << 20); fill_xorshift(m, 0x10000000, 2u << 20, 99);
  const uint64_t changed[] = {0x10000100, 0x100ff000, 0x101f0000};
  for (unsigned w : {1u, 8u}) {
    Engine e(m, {.value_cap = 4'000'000, .block_size = 1u << 20, .workers = w}); ScanStats s; std::vector<GroupInfo> g;
    REQUIRE(e.scan({G("q", U64, 1, 0, range(-1, 2e19))}, false, s)[0].has_values);
    CHECK(e.narrow({{"q", {Pred::EqPrev}}}, true, g, s)); CHECK(g[0].count == (2u << 20) - 7); CHECK(g[0].has_values);
    CHECK(e.peak_narrow_held_bytes() <= (w + 4) * (1u << 20));
    for (uint64_t at : changed) m.poke<uint8_t>(at, m.peek<uint8_t>(at) ^ 0xff);
    CHECK(e.narrow({{"q", {Pred::NePrev}}}, true, g, s)); CHECK(g[0].count == 24);   // the 8 places over each byte
    for (uint64_t at : changed) m.poke<uint8_t>(at, m.peek<uint8_t>(at) ^ 0xff);
  }
}
TEST_CASE("the room runs hold stays within its bound when blocks are split across runs") {
  // the steps of the first narrow test above, whose 1 MiB blocks are split by stretches of 0xff
  const Pred tiny = range(-1e-30, 1e-30);
  for (unsigned w : {1u, 2u}) {
    FakeMemory m; mixed(m);
    Engine e(m, {.value_cap = 2'000'000, .block_size = 1u << 20, .workers = w}); ScanStats s; std::vector<GroupInfo> g;
    e.scan({G("i", I32, 4, 0, eq(3)), G("f", F32, 4, 0, tiny), G("b", U8, 1, 0, eq(0x5a))}, false, s);
    spread(m, 8191 * 4, 0, 5, true); m.remove_region(kMixed[1][0]);
    Pred moved = range(-10, 10); moved.moved = true;
    CHECK(e.narrow({{"i", moved}, {"f", tiny}, {"b", {Pred::EqPrev}}}, true, g, s));
    CHECK(e.peak_narrow_held_bytes() <= (w + 4) * (1u << 20));
    spread(m, 8191 * 4, 0, 4, false);
    CHECK(e.narrow({{"b", {Pred::EqPrev}}, {"i", {Pred::LtPrev}}, {"f", eq(0)}}, true, g, s)); CHECK(g[2].has_values);
    CHECK(e.peak_narrow_held_bytes() > 0); CHECK(e.peak_narrow_held_bytes() <= (w + 4) * (1u << 20));
  }
}
TEST_CASE("a group that passes value_cap part way through a narrow, and one that ends at it, end the same on any worker count") {
  // c: i32 7 at every place of 4 MiB, four 1 MiB runs; 48,576 of them change, so exactly 1,000,000
  // (value_cap) are left and c gains values. a: i32 0 at every place of 8 MiB, eight runs, which
  // passes the cap in its fourth run.
  using Outcome = std::tuple<std::vector<std::tuple<uint64_t, bool>>, uint64_t, std::vector<std::vector<std::pair<uint64_t, uint64_t>>>, uint64_t>;
  auto place = [](uint64_t i) { return 0x10000000 + i * 4; };
  auto run = [&](unsigned w) {
    FakeMemory m; m.add_region(0x10000000, 4u << 20); m.add_region(0x20000000, 8u << 20);
    std::vector<int32_t> sevens(1u << 20, 7); REQUIRE(m.write(0x10000000, sevens.data(), 4u << 20));
    Engine e(m, {.value_cap = 1'000'000, .block_size = 1u << 20, .workers = w}); ScanStats s; std::vector<GroupInfo> g;
    const auto sc = e.scan({G("c", I32, 4, 0, eq(7)), G("a", I32, 4, 0, eq(0))}, false, s);
    REQUIRE(sc[0].count == (1u << 20)); REQUIRE(sc[1].count == (2u << 20)); REQUIRE_FALSE(sc[0].has_values);
    for (uint64_t k = 0; k < 48'576; k++) m.poke<int32_t>(place(k * 21), 8);
    Outcome o;
    CHECK(e.narrow({{"c", eq(7)}, {"a", eq(0)}}, true, g, s));
    for (const GroupInfo& i : g) std::get<0>(o).emplace_back(i.count, i.has_values);
    std::get<1>(o) = e.storage_bytes();
    std::get<2>(o) = rows({e.peek("c", 0, 1u << 30), e.peek("a", 0, 1u << 30)});
    for (uint64_t i = 1'030'000; i < 1'030'100; i++) m.poke<int32_t>(place(i), 6);   // in the last run
    m.poke<int32_t>(place(100'001), 6);                                             // in the first
    CHECK(e.narrow({{"c", {Pred::LtPrev}}}, true, g, s)); std::get<3>(o) = g[0].count;
    return o;
  };
  const Outcome one = run(1);
  REQUIRE(std::get<0>(one).size() == 2);
  CHECK(std::get<0>(one)[0] == std::tuple<uint64_t, bool>{1'000'000, true});
  CHECK(std::get<0>(one)[1] == std::tuple<uint64_t, bool>{2u << 20, false});
  CHECK(std::get<3>(one) == 101);
  for (unsigned w : {2u, 4u, 8u}) {
    const Outcome other = run(w);
    CHECK(std::get<0>(other) == std::get<0>(one)); CHECK(std::get<1>(other) == std::get<1>(one));
    CHECK(std::get<2>(other) == std::get<2>(one)); CHECK(std::get<3>(other) == std::get<3>(one));
  }
}
TEST_CASE("a narrow on several workers that is refused, or whose read throws, changes nothing") {
  ThrowingMemory m; m.add_region(0x10000000, 16u << 20);              // zeros: every place matches
  Engine e(m, {.block_size = 1u << 20, .workers = 4}); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("i", I32, 4, 0, eq(0)), G("b", U8, 1, 0, eq(0))}, false, s);
  const uint64_t storage = e.storage_bytes();
  auto unchanged = [&] {
    REQUIRE(e.groups().size() == 2); CHECK(e.groups()[0].count == (4u << 20)); CHECK(e.groups()[1].count == (16u << 20));
    CHECK(e.groups()[0].has_values); CHECK(e.groups()[1].has_values); CHECK(e.storage_bytes() == storage);
  };
  CHECK_FALSE(e.narrow({{"i", eq(1)}, {"b", eq(1)}}, true, g, s)); CHECK(s.lost == 0); unchanged();
  m.reads_left = 5;                                                   // a read part way through throws
  CHECK_THROWS_AS(e.narrow({{"i", {Pred::EqPrev}}, {"b", eq(1)}}, false, g, s), std::runtime_error); unchanged();
  m.reads_left = 1 << 30; m.poke<int32_t>(0x10000000 + 0x123450, 9);
  CHECK(e.narrow({{"i", {Pred::EqPrev}}}, true, g, s)); CHECK(g[0].count == (4u << 20) - 1);
}
