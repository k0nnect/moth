#include "engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "engine/block_scan.hpp"

namespace moth {

namespace {

// A block followed by more of its region is read this much longer, so a value of up to 8 bytes
// starting in its last bytes is seen whole. scan_block's limit keeps it from being found twice.
constexpr size_t kOverlap = 7;

void validate(const std::vector<GroupSpec>& specs) {
  for (size_t i = 0; i < specs.size(); i++) {
    const GroupSpec& g = specs[i];
    if (g.pred.op != Pred::Eq && g.pred.op != Pred::Range)
      throw EngineError("bad_request", "a first scan needs a value or a range, not a comparison with the last one");
    if (g.align == 0) throw EngineError("bad_request", "align must be positive");
    for (size_t j = 0; j < i; j++)
      if (specs[j].id == g.id) throw EngineError("bad_request", "group listed twice: " + g.id);
  }
}

bool listed(const std::vector<GroupSpec>& specs, const std::string& id) {
  return std::any_of(specs.begin(), specs.end(), [&](const GroupSpec& s) { return s.id == id; });
}

}  // namespace

Engine::Engine(ProcessMemory& mem, EngineConfig cfg) : mem_(mem), cfg_(cfg) {
  if (cfg_.block_size == 0 || cfg_.block_size > (size_t{1} << 30))
    throw EngineError("bad_request", "block_size must be between 1 byte and 1 GiB");
}

std::vector<GroupInfo> Engine::scan(const std::vector<GroupSpec>& specs, bool keep, ScanStats& stats) {
  const auto start = std::chrono::steady_clock::now();
  stats = ScanStats{};
  validate(specs);
  if (!mem_.alive()) throw EngineError("target_gone", "the game has closed");

  // Free the groups being replaced first, so old and new candidates are never held together.
  if (keep)
    std::erase_if(groups_, [&](const Group& g) { return listed(specs, g.spec.id); });
  else
    groups_.clear();
  const uint64_t kept = storage_bytes();
  std::vector<Group> fresh;
  fresh.reserve(specs.size());
  for (const GroupSpec& s : specs) fresh.emplace_back(s);

  std::vector<Region> regions = mem_.writable_regions();
  std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) { return a.base < b.base; });
  std::vector<uint8_t> buf(cfg_.block_size + kOverlap);
  std::vector<std::vector<uint32_t>> hits(specs.size());

  for (const Region& r : regions) {
    stats.regions++;
    for (uint64_t off = 0; off < r.size; off += cfg_.block_size) {
      const uint64_t left = r.size - off;
      const size_t span = static_cast<size_t>(std::min<uint64_t>(cfg_.block_size, left));
      const size_t want = static_cast<size_t>(std::min<uint64_t>(span + kOverlap, left));
      const size_t got = std::min(mem_.read(r.base + off, buf.data(), want), want);
      const size_t limit = std::min(span, got);  // a short read searches what arrived
      stats.bytes += limit;
      stats.unreadable_bytes += span - limit;
      if (limit == 0) continue;
      scan_block(buf.data(), got, limit, r.base + off, specs, hits);
      for (size_t i = 0; i < fresh.size(); i++) {
        add_hits(fresh[i], r.base + off, span, buf.data(), hits[i]);
        hits[i].clear();
      }
      // Positions alone never shrink, so untracking on them now matches what the final check
      // would decide, and keeps memory near the budget during the walk instead of after it.
      fit_budget(fresh, kept, false);
    }
  }
  if (!mem_.alive()) throw EngineError("target_gone", "the game has closed");
  fit_budget(fresh, kept, true);

  std::vector<GroupInfo> out;
  out.reserve(fresh.size());
  for (Group& g : fresh) {
    out.push_back(info(g));
    groups_.push_back(std::move(g));
  }
  stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return out;
}

void Engine::add_hits(Group& g, uint64_t base, size_t span, const uint8_t* data,
                      const std::vector<uint32_t>& offsets) {
  g.count += offsets.size();
  if (!g.tracked) return;
  g.positions.add_block(base, static_cast<uint32_t>(span), offsets);
  if (!g.has_values) return;
  if (g.count > cfg_.value_cap) {
    std::vector<uint64_t>().swap(g.values);
    g.has_values = false;
    return;
  }
  const size_t w = width_of(g.spec.type);
  for (uint32_t o : offsets) {
    uint64_t v = 0;
    std::memcpy(&v, data + o, w);  // little-endian host
    g.values.push_back(v);
  }
}

// Untracks fresh groups from the last one backwards until kept plus their storage fits the
// budget. Without values the storage counted is a lower bound of the final one.
void Engine::fit_budget(std::vector<Group>& fresh, uint64_t kept, bool with_values) {
  auto size = [&](const Group& g) { return with_values ? bytes_of(g) : g.positions.bytes(); };
  uint64_t total = kept;
  for (const Group& g : fresh) total += size(g);
  for (size_t i = fresh.size(); i-- > 0 && total > cfg_.budget_bytes;) {
    if (!fresh[i].tracked) continue;
    total -= size(fresh[i]);
    untrack(fresh[i]);
  }
}

std::vector<Item> Engine::peek(const std::string& id, uint64_t offset, uint64_t limit) {
  const Group& g = find(id);
  std::vector<Item> items;
  if (!g.tracked) return items;
  const size_t w = width_of(g.spec.type);
  for (uint64_t addr : g.positions.addresses(offset, limit)) {
    uint64_t bits = 0;
    if (mem_.read(addr, &bits, w) == w) items.push_back({addr, bits});  // little-endian host
  }
  return items;
}

std::vector<GroupInfo> Engine::groups() const {
  std::vector<GroupInfo> out;
  out.reserve(groups_.size());
  for (const Group& g : groups_) out.push_back(info(g));
  return out;
}

uint64_t Engine::storage_bytes() const {
  uint64_t total = 0;
  for (const Group& g : groups_) total += bytes_of(g);
  return total;
}

GroupInfo Engine::info(const Group& g) { return {g.spec.id, g.count, g.tracked, g.has_values}; }

uint64_t Engine::bytes_of(const Group& g) { return g.positions.bytes() + g.values.capacity() * sizeof(uint64_t); }

void Engine::untrack(Group& g) {
  g.positions = PositionSet(g.spec.align);
  std::vector<uint64_t>().swap(g.values);
  g.tracked = false;
  g.has_values = false;
}

const Engine::Group& Engine::find(const std::string& id) const {
  for (const Group& g : groups_)
    if (g.spec.id == id) return g;
  throw EngineError("no_such_group", "no group named " + id);
}

}  // namespace moth
