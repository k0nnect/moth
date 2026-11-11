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

// A narrow reads a group's positions in runs: a run ends at the end of a block, or where the next
// position is more than this far past the previous one.
constexpr uint32_t kRunGap = 64u << 10;

bool compares_with_last(const Pred& p) {
  return p.op == Pred::LtPrev || p.op == Pred::GtPrev || p.op == Pred::EqPrev || p.op == Pred::NePrev;
}

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

bool Engine::narrow(const std::vector<NarrowItem>& items, bool atomic, std::vector<GroupInfo>& out,
                    ScanStats& stats) {
  const auto start = std::chrono::steady_clock::now();
  stats = ScanStats{};
  std::vector<size_t> at;  // each listed group's index in groups_
  at.reserve(items.size());
  for (const NarrowItem& it : items) {
    const size_t k = index_of(it.id);
    const Group& g = groups_[k];
    if (!g.tracked)
      throw EngineError("no_such_group",
                        "group " + it.id + " is untracked, so it has no positions to narrow; scan it again");
    if (std::find(at.begin(), at.end(), k) != at.end())
      throw EngineError("bad_request", "group listed twice: " + it.id);
    if (compares_with_last(it.pred) && !g.has_values)
      throw EngineError("no_values", "group " + it.id + " has no stored values");
    at.push_back(k);
  }
  if (!mem_.alive()) throw EngineError("target_gone", "the game has closed");

  // The replacements are built beside the groups, so nothing changes until the narrow applies.
  std::vector<Group> fresh;
  fresh.reserve(items.size());
  std::vector<uint8_t> buf;
  uint64_t total = 0;
  for (size_t i = 0; i < items.size(); i++) {
    fresh.push_back(narrowed(groups_[at[i]], items[i].pred, buf, stats));
    total += fresh.back().count;
  }
  // Reads also fail once the game closes; that is reported, not taken as lost matches.
  if (!mem_.alive()) throw EngineError("target_gone", "the game has closed");

  const bool applied = !atomic || total > 0;
  if (applied) {
    fit_gained_values(fresh, at);
    for (size_t i = 0; i < fresh.size(); i++) groups_[at[i]] = std::move(fresh[i]);
  } else {
    stats.lost = 0;  // nothing was dropped
  }
  out.clear();
  for (size_t k : at) out.push_back(info(groups_[k]));
  stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return applied;
}

// g's positions whose value now matches p, with values captured while the count is within
// value_cap. Each run is read with one call; positions its read did not reach are counted lost.
Engine::Group Engine::narrowed(const Group& g, const Pred& p, std::vector<uint8_t>& buf, ScanStats& stats) {
  struct Run { uint32_t first, last; };
  const ValueType t = g.spec.type;
  const size_t w = width_of(t);
  Group out(g.spec);
  uint64_t index = 0;  // of the position being tested, in g, which is also its stored value's index
  std::vector<Run> runs;
  std::vector<uint32_t> keep;
  for (const PositionSet::Block& b : g.positions.blocks()) {
    runs.clear();
    g.positions.for_each_in(b, [&](uint32_t o) {
      if (runs.empty() || o - runs.back().last > kRunGap) runs.push_back({o, o});
      else runs.back().last = o;
    });
    keep.clear();
    size_t r = 0, got = 0;
    g.positions.for_each_in(b, [&](uint32_t o) {
      if (o > runs[r].last) r++;
      const Run& run = runs[r];
      if (o == run.first) {
        const size_t want = run.last - run.first + w;
        if (buf.size() < want) buf.resize(want);
        got = std::min(mem_.read(b.base + run.first, buf.data(), want), want);
        stats.bytes += got;
      }
      const uint64_t prev = g.has_values ? g.values[static_cast<size_t>(index)] : 0;
      index++;
      const size_t off = o - run.first;
      if (off + w > got) {
        stats.lost++;
        return;
      }
      uint64_t cur = 0;
      std::memcpy(&cur, buf.data() + off, w);  // little-endian host
      if (!matches(p, t, cur, prev)) return;
      keep.push_back(o);
      out.count++;
      if (!out.has_values) return;
      if (out.count > cfg_.value_cap) {
        std::vector<uint64_t>().swap(out.values);
        out.has_values = false;
        return;
      }
      out.values.push_back(cur);
    });
    out.positions.add_block(b.base, b.span, keep);
  }
  out.values.shrink_to_fit();
  return out;
}

// Values a narrowed group gains (it had none, and is now within value_cap) are kept, in request
// order, only while all storage stays within budget_bytes. Values a group already had only shrink.
void Engine::fit_gained_values(std::vector<Group>& fresh, const std::vector<size_t>& at) {
  auto gains = [&](size_t i) { return fresh[i].has_values && !groups_[at[i]].has_values; };
  uint64_t total = storage_bytes();
  for (size_t i = 0; i < fresh.size(); i++)
    total = total - bytes_of(groups_[at[i]]) + (gains(i) ? fresh[i].positions.bytes() : bytes_of(fresh[i]));
  for (size_t i = 0; i < fresh.size(); i++) {
    if (!gains(i)) continue;
    const uint64_t v = fresh[i].values.capacity() * sizeof(uint64_t);
    if (total + v <= cfg_.budget_bytes) {
      total += v;
      continue;
    }
    std::vector<uint64_t>().swap(fresh[i].values);
    fresh[i].has_values = false;
  }
}

void Engine::drop(const std::vector<std::string>& ids) {
  for (const std::string& id : ids) find(id);  // throws for an unknown id before anything is removed
  std::erase_if(groups_, [&](const Group& g) { return std::find(ids.begin(), ids.end(), g.spec.id) != ids.end(); });
}

bool Engine::read_value(uint64_t addr, ValueType type, uint64_t& bits) {
  if (!mem_.alive()) throw EngineError("target_gone", "the game has closed");
  const size_t w = width_of(type);
  uint64_t v = 0;
  if (mem_.read(addr, &v, w) != w) return false;
  bits = v;  // little-endian host
  return true;
}

bool Engine::write_value(uint64_t addr, ValueType type, uint64_t bits, uint64_t& prev_bits) {
  uint64_t prev = 0;
  if (!read_value(addr, type, prev)) return false;
  if (!mem_.write(addr, &bits, width_of(type))) return false;  // the low bytes, little-endian host
  prev_bits = prev;
  return true;
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

size_t Engine::index_of(const std::string& id) const {
  for (size_t i = 0; i < groups_.size(); i++)
    if (groups_[i].spec.id == id) return i;
  throw EngineError("no_such_group", "no group named " + id);
}

const Engine::Group& Engine::find(const std::string& id) const { return groups_[index_of(id)]; }

}  // namespace moth
