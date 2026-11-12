// Engine speed on this machine: a first scan and one narrow over a seeded 256 MiB FakeMemory, with
// the twelve groups a search for the whole number 100 makes; then a narrow over a dense group, every
// aligned 4-byte place of the same memory, by an EqPrev step, on one worker and on the automatic
// count. Prints the best of three rounds as
//   scan: <x> s/GB, narrow: <y> ms
//   dense narrow: <a> s/GB on 1 worker, <b> s/GB on <n> workers
// where GB is 2^30 bytes. Optional argument: the worker count for the first line (default 0, auto,
// as the helper uses). Reading here is a memcpy, so the figures are the engine's own share of a
// step, not a game's.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "engine/engine.hpp"
#include "test/fake_memory.hpp"
#include "test/test_util.hpp"

using namespace moth;
using namespace moth::test;
using enum ValueType;

namespace {

// Seconds per GiB of a step.
double s_per_gb(const ScanStats& s) { return s.ms / 1000.0 / (static_cast<double>(s.bytes) / (1ull << 30)); }

// The best of three EqPrev narrows over every aligned 4-byte place of m, which nothing changes, so
// each keeps all 67,108,864 places and stores their values again.
double dense_narrow(FakeMemory& m, unsigned workers, unsigned& used) {
  Engine e(m, {.value_cap = 1u << 27, .workers = workers});  // over the default cap, so it keeps values
  used = e.workers_used();
  ScanStats s;
  e.scan({G("all", I32, 4, 0, range(-2147483649.0, 2147483648.0))}, false, s);
  double best = 1e30;
  for (int round = 0; round < 3; round++) {
    std::vector<GroupInfo> out;
    e.narrow({{"all", {Pred::EqPrev}}}, true, out, s);
    best = std::min(best, s_per_gb(s));
  }
  return best;
}

}  // namespace

int main(int argc, char** argv) {
  const unsigned workers = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 0;
  constexpr uint64_t base = 0x10000000;
  constexpr size_t size = size_t{256} << 20;
  FakeMemory m;
  m.add_region(base, size);
  fill_xorshift(m, base, size, 0x6d6f7468);

  const std::vector<GroupSpec> groups = {
      G("i32", I32, 4, 0, eq(100)),        G("f32", F32, 4, 0, range(99, 101)),
      G("i16", I16, 2, 0, eq(100)),        G("i64", I64, 8, 0, eq(100)),
      G("f64", F64, 8, 0, range(99, 101)), G("u8", U8, 1, 0, eq(100)),
      G("i32x100", I32, 4, 0, eq(10000)),  G("i32x10", I32, 4, 0, eq(1000)),
      G("i32x1000", I32, 4, 0, eq(100000)), G("f32pct", F32, 4, 0, range(0.99, 1.01)),
      G("i32u", I32, 1, 4, eq(100)),       G("f32u", F32, 1, 4, range(99, 101))};
  std::vector<NarrowItem> again;
  for (const GroupSpec& g : groups) again.push_back({g.id, g.pred});

  double scan_s_per_gb = 1e30, narrow_ms = 1e30;
  for (int round = 0; round < 3; round++) {
    Engine e(m, {.workers = workers});
    ScanStats scan, narrow;
    e.scan(groups, false, scan);
    std::vector<GroupInfo> out;
    e.narrow(again, true, out, narrow);
    scan_s_per_gb = std::min(scan_s_per_gb, s_per_gb(scan));
    narrow_ms = std::min(narrow_ms, narrow.ms);
  }
  std::printf("scan: %.3f s/GB, narrow: %.1f ms\n", scan_s_per_gb, narrow_ms);

  unsigned one = 0, many = 0;
  const double dense_one = dense_narrow(m, 1, one), dense_auto = dense_narrow(m, 0, many);
  std::printf("dense narrow: %.3f s/GB on %u worker, %.3f s/GB on %u workers\n", dense_one, one, dense_auto, many);
  return 0;
}
