#include "engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <thread>

#include "engine/block_scan.hpp"

namespace moth {

namespace {

// A block followed by more of its region is read this much longer, so a value of up to 8 bytes
// starting in its last bytes is seen whole. scan_block's limit keeps it from being found twice.
constexpr size_t kOverlap = 7;

// A narrow reads positions in runs: a run ends at the end of a block, or where the next position,
// of any listed group, is more than this far past the previous one.
constexpr uint32_t kRunGap = 64u << 10;

// A worker searches a block this many bytes at a time and compacts each slice's matches into the
// block's positions before the next, so a value matching everywhere (0) never has a whole block's
// raw offsets held at once. Smaller slices saved no more memory and cost speed.
constexpr size_t kSlice = size_t{256} << 10;

// At most this many blocks that have been read wait for a worker.
constexpr size_t kQueue = 4;

// Memory becomes unreadable a page at a time.
constexpr uint64_t kPage = 4096;

// The bytes of a buffer that were read, as [first, second) offsets: ascending, apart.
using Spans = std::vector<std::pair<size_t, size_t>>;

void add_span(Spans& spans, size_t from, size_t to) {
  if (from == to) return;
  if (!spans.empty() && spans.back().second == from)
    spans.back().second = to;
  else
    spans.push_back({from, to});
}

// Reads n bytes at addr into buf + at and appends the offsets that arrived to spans. Everything
// readable costs one read. A read that comes back short is continued from where it stopped; one
// that gets nothing and covers more than one page is split in two at a page boundary near its
// middle, and each half read the same way. So with a read that gets all or nothing (as
// ReadProcessMemory does), an unreadable page loses only its own bytes, at about two reads per
// halving. Splitting stops once the process has closed. Returns the bytes read.
size_t read_spans(ProcessMemory& mem, uint64_t addr, uint8_t* buf, size_t at, size_t n, Spans& spans) {
  size_t total = 0;
  while (n > 0) {
    const size_t got = std::min(mem.read(addr, buf + at, n), n);
    add_span(spans, at, at + got);
    if (got == 0) break;
    total += got;
    addr += got;
    at += got;
    n -= got;
  }
  if (n == 0) return total;
  const uint64_t first = addr - addr % kPage + kPage;  // the first page boundary after addr
  const uint64_t last = (addr + n - 1) - (addr + n - 1) % kPage;  // the last one before the end
  if (first > last || !mem.alive()) return total;      // one page, or the game is gone
  const uint64_t mid = std::clamp<uint64_t>((addr + n / 2) - (addr + n / 2) % kPage, first, last);
  const size_t left = static_cast<size_t>(mid - addr);
  total += read_spans(mem, addr, buf, at, left, spans);
  return total + read_spans(mem, mid, buf, at + left, n - left, spans);
}

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

// 0 means one thread fewer than the hardware has, kept between 1 and 8.
unsigned resolve_workers(unsigned workers) {
  if (workers != 0) return workers;
  const int hardware = static_cast<int>(std::thread::hardware_concurrency());  // 0 when unknown
  return static_cast<unsigned>(std::max(1, std::min(8, hardware - 1)));
}

// A byte buffer that is always read into before use, so it is never zero-filled.
class Bytes {
 public:
  uint8_t* data() { return p_.get(); }
  // At least n bytes. Growing does not keep the old contents.
  uint8_t* reserve(size_t n) {
    if (n > n_) {
      p_.reset(new uint8_t[n]);
      n_ = n;
    }
    return p_.get();
  }

 private:
  std::unique_ptr<uint8_t[]> p_;
  size_t n_ = 0;
};

// One block of a scan: read by the calling thread, then searched by a worker.
struct Job {
  uint64_t base = 0;
  uint32_t span = 0;       // the block's own bytes; data holds up to kOverlap more
  Spans parts;             // the bytes of data that were read
  Bytes data;
  std::vector<PositionSet::Block> found;  // per group, set by the worker
  bool searched = false;
};

// The bytes of a job's block, not its overlap, that were read.
uint64_t block_bytes(const Job& j) {
  uint64_t n = 0;
  for (const auto& [from, to] : j.parts) n += from < j.span ? std::min<size_t>(to, j.span) - from : 0;
  return n;
}

// Searches the parts of a job's block that were read, kSlice bytes at a time, compacting each
// slice's matches before the next. A value is found only when all its bytes were read. hits is
// the worker's own scratch for one slice's raw offsets.
void search(Job& j, std::span<const GroupSpec> groups, std::vector<std::vector<uint32_t>>& hits) {
  std::vector<PositionSet::Builder> found;
  found.reserve(groups.size());
  for (const GroupSpec& g : groups) found.emplace_back(g.align, j.base, j.span);
  for (const auto& [from, to] : j.parts) {
    const size_t limit = std::min<size_t>(to, j.span);  // positions start in the block's own bytes
    for (size_t s = from; s < limit; s += kSlice) {
      // the slice's values may run on into the bytes after it, up to the end of the part
      scan_block(j.data.data() + s, to - s, std::min(kSlice, limit - s), j.base + s, groups, hits);
      for (size_t i = 0; i < groups.size(); i++) {
        found[i].add(hits[i], static_cast<uint32_t>(s));
        hits[i].clear();
      }
    }
  }
  j.found.clear();
  for (PositionSet::Builder& b : found) j.found.push_back(b.finish());
}

// Worker threads that search a scan's blocks. Only the calling thread submits and takes jobs: it
// submits them in address order and takes them back searched, in the same order. It submits only
// while fewer than workers + kQueue jobs are out, so at most kQueue wait while every worker holds
// one. Destruction stops the workers after their current block and joins them; an exception a
// worker throws is rethrown by take.
class Searchers {
 public:
  Searchers(std::span<const GroupSpec> groups, unsigned workers, size_t job_bytes)
      : groups_(groups), job_bytes_(job_bytes), max_out_(workers + kQueue) {
    try {
      for (unsigned i = 0; i < workers; i++) threads_.emplace_back([this] { work(); });
    } catch (...) {
      stop();
      throw;
    }
  }
  ~Searchers() { stop(); }
  Searchers(const Searchers&) = delete;
  Searchers& operator=(const Searchers&) = delete;

  bool full() const { return out_.size() >= max_out_; }

  // A job to read into, recycled or new. Call only when not full.
  Job* acquire() {
    if (idle_.empty()) {
      jobs_.push_back(std::make_unique<Job>());
      idle_.push_back(jobs_.back().get());
    }
    Job* j = idle_.back();
    idle_.pop_back();
    j->data.reserve(job_bytes_);
    return j;
  }
  void release(Job* j) { idle_.push_back(j); }  // a job taken back, or never submitted

  void submit(Job* j) {
    out_.push_back(j);
    {
      std::lock_guard lock(mu_);
      j->searched = false;
      waiting_.push_back(j);
    }
    work_cv_.notify_one();
  }

  // The oldest job out once it is searched; null if none is out, or if it is not searched yet and
  // wait is false.
  Job* take(bool wait) {
    if (out_.empty()) return nullptr;
    Job* j = out_.front();
    {
      std::unique_lock lock(mu_);
      if (wait) done_cv_.wait(lock, [&] { return j->searched || error_; });
      if (error_) std::rethrow_exception(error_);
      if (!j->searched) return nullptr;
    }
    out_.pop_front();
    return j;
  }

  // Joins the workers and returns the raw offsets they had room for, which is the most they held
  // at once: each one's scratch only grows while it runs. Called once every job is taken back.
  uint64_t finish() {
    stop();
    return raw_offsets_;
  }

 private:
  void work() {
    std::vector<std::vector<uint32_t>> hits(groups_.size());
    try {
      for (;;) {
        Job* j;
        {
          std::unique_lock lock(mu_);
          work_cv_.wait(lock, [&] { return stopping_ || !waiting_.empty(); });
          if (stopping_) break;
          j = waiting_.front();
          waiting_.pop_front();
        }
        search(*j, groups_, hits);
        {
          std::lock_guard lock(mu_);
          j->searched = true;
        }
        done_cv_.notify_one();
      }
    } catch (...) {
      {
        std::lock_guard lock(mu_);
        if (!error_) error_ = std::current_exception();
      }
      done_cv_.notify_one();
    }
    uint64_t room = 0;
    for (const std::vector<uint32_t>& h : hits) room += h.capacity();
    std::lock_guard lock(mu_);
    raw_offsets_ += room;
  }

  void stop() {
    {
      std::lock_guard lock(mu_);
      stopping_ = true;
    }
    work_cv_.notify_all();
    for (std::thread& t : threads_)
      if (t.joinable()) t.join();
  }

  const std::span<const GroupSpec> groups_;
  const size_t job_bytes_;
  const size_t max_out_;
  // the calling thread's own
  std::vector<std::unique_ptr<Job>> jobs_;
  std::vector<Job*> idle_;
  std::deque<Job*> out_;  // submitted and not yet taken back, oldest first
  // shared with the workers, under mu_
  std::mutex mu_;
  std::condition_variable work_cv_, done_cv_;
  std::deque<Job*> waiting_;  // submitted and not yet picked up by a worker
  bool stopping_ = false;
  std::exception_ptr error_;
  uint64_t raw_offsets_ = 0;
  std::vector<std::thread> threads_;
};

}  // namespace

Engine::Engine(ProcessMemory& mem, EngineConfig cfg) : mem_(mem), cfg_(cfg), workers_(resolve_workers(cfg.workers)) {
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

  peak_raw_offsets_ = 0;
  Searchers searchers(specs, workers_, cfg_.block_size + kOverlap);
  // Adds the oldest searched block's matches to the groups, in address order whatever the
  // worker count. Returns false when no block is out, or wait is false and it is not searched yet.
  auto add_next = [&](bool wait) {
    Job* j = searchers.take(wait);
    if (j == nullptr) return false;
    for (size_t i = 0; i < fresh.size(); i++) add_found(fresh[i], std::move(j->found[i]), j->data.data());
    // Positions alone never shrink, so untracking on them now matches what the final check
    // would decide, and keeps memory near the budget during the walk instead of after it.
    fit_budget(fresh, kept, false);
    searchers.release(j);
    return true;
  };
  for (const Region& r : regions) {
    stats.regions++;
    for (uint64_t off = 0; off < r.size; off += cfg_.block_size) {
      while (searchers.full()) add_next(true);
      Job* j = searchers.acquire();
      const uint64_t left = r.size - off;
      j->base = r.base + off;
      j->span = static_cast<uint32_t>(std::min<uint64_t>(cfg_.block_size, left));
      const size_t want = static_cast<size_t>(std::min<uint64_t>(j->span + kOverlap, left));
      j->parts.clear();
      read_spans(mem_, j->base, j->data.data(), 0, want, j->parts);  // only what arrived is searched
      const uint64_t got = block_bytes(*j);
      stats.bytes += got;
      stats.unreadable_bytes += j->span - got;
      if (got == 0) {
        searchers.release(j);
        continue;
      }
      searchers.submit(j);
      while (add_next(false)) {
      }
    }
  }
  while (add_next(true)) {
  }
  peak_raw_offsets_ = searchers.finish();
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

// Adds a block's matches to g: always to its count; to its positions while it is tracked; and
// their values, read from the block's bytes, while its count is within value_cap.
void Engine::add_found(Group& g, PositionSet::Block b, const uint8_t* data) {
  g.count += b.count;
  if (!g.tracked) return;
  if (g.has_values && g.count > cfg_.value_cap) {
    std::vector<uint64_t>().swap(g.values);
    g.has_values = false;
  }
  if (g.has_values) {
    const size_t w = width_of(g.spec.type);
    g.positions.for_each_in(b, [&](uint32_t o) {
      uint64_t v = 0;
      std::memcpy(&v, data + o, w);  // little-endian host
      g.values.push_back(v);
    });
  }
  g.positions.append(std::move(b));
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
  std::vector<Group> fresh = narrowed(at, items, stats);
  uint64_t total = 0;
  for (const Group& g : fresh) total += g.count;
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

// The listed groups' replacements, in request order: each group's positions whose value now matches
// its predicate, with values captured while its count is within value_cap.
// Groups from one scan share their blocks, so a block is narrowed together with every listed
// group's block at the same base and span: their positions are merged into runs, each run is read
// with one call while all of it is readable, and every group is tested on the same bytes. Blocks
// that differ (groups from scans of a changed memory map) are read on their own. A position whose
// bytes could not be read is lost.
std::vector<Engine::Group> Engine::narrowed(const std::vector<size_t>& at, const std::vector<NarrowItem>& items,
                                            ScanStats& stats) {
  struct Lane {
    const Group* old;
    const Pred* pred;
    Group out;
    size_t block = 0;    // the next of old's blocks to narrow
    uint64_t index = 0;  // of the next position in old, which is also its stored value's index
  };
  // Positions first..last of a block (offsets), whose values take bytes [first, end).
  struct Run {
    uint32_t first, last, end;
    size_t at = 0;       // where its bytes are in the buffer
    bool whole = false;  // all of them were read
  };
  std::vector<Lane> lanes;
  lanes.reserve(at.size());
  for (size_t i = 0; i < at.size(); i++) lanes.push_back({&groups_[at[i]], &items[i].pred, Group(groups_[at[i]].spec)});
  std::vector<size_t> set;  // the lanes narrowing the current block
  std::vector<Run> spans, runs;
  Spans arrived;            // the buffer bytes that were read, run after run
  Bytes buf;
  for (;;) {
    // the lowest next block among the lanes, and every lane whose next block is the same
    const PositionSet::Block* head = nullptr;
    set.clear();
    for (size_t i = 0; i < lanes.size(); i++) {
      const std::vector<PositionSet::Block>& blocks = lanes[i].old->positions.blocks();
      if (lanes[i].block == blocks.size()) continue;
      const PositionSet::Block& b = blocks[lanes[i].block];
      if (head != nullptr && b.base == head->base && b.span == head->span) {
        set.push_back(i);
      } else if (head == nullptr || b.base < head->base || (b.base == head->base && b.span < head->span)) {
        head = &b;
        set.assign(1, i);
      }
    }
    if (head == nullptr) break;
    const uint64_t base = head->base;

    // Each lane's own runs, then merged: two join when one starts within kRunGap of the other's
    // last position, which is the run rule applied to all the lanes' positions together.
    spans.clear();
    for (size_t i : set) {
      const Group& g = *lanes[i].old;
      const uint32_t w = static_cast<uint32_t>(width_of(g.spec.type));
      const size_t from = spans.size();
      g.positions.for_each_in(g.positions.blocks()[lanes[i].block], [&](uint32_t o) {
        if (spans.size() == from || o - spans.back().last > kRunGap) {
          spans.push_back({o, o, o + w});
        } else {
          spans.back().last = o;
          spans.back().end = o + w;
        }
      });
    }
    if (set.size() > 1) std::sort(spans.begin(), spans.end(), [](const Run& a, const Run& b) { return a.first < b.first; });
    runs.clear();
    for (const Run& s : spans) {
      if (!runs.empty() && s.first <= runs.back().last + kRunGap) {
        runs.back().last = std::max(runs.back().last, s.last);
        runs.back().end = std::max(runs.back().end, s.end);
      } else {
        runs.push_back(s);
      }
    }

    // each run read (with one call unless part of it is unreadable), packed one after another
    // into the buffer
    size_t need = 0;
    for (Run& r : runs) {
      r.at = need;
      need += r.end - r.first;
    }
    uint8_t* data = buf.reserve(need);
    arrived.clear();
    for (Run& r : runs) {
      const size_t got = read_spans(mem_, base + r.first, data, r.at, r.end - r.first, arrived);
      r.whole = got == r.end - r.first;
      stats.bytes += got;
    }

    for (size_t i : set) {
      Lane& lane = lanes[i];
      const Group& g = *lane.old;
      const PositionSet::Block& b = g.positions.blocks()[lane.block++];
      const ValueType t = g.spec.type;
      const size_t w = width_of(t);
      Group& out = lane.out;
      PositionSet::Builder keep(g.spec.align, b.base, b.span);
      // the run holding the position, and the first span of arrived that ends at or past its
      // value's bytes: both only move forward, since the value's place in the buffer does
      size_t r = 0, k = 0;
      g.positions.for_each_in(b, [&](uint32_t o) {
        while (runs[r].last < o) r++;  // every position is in a run; other lanes' runs are skipped
        const uint64_t prev = g.has_values ? g.values[static_cast<size_t>(lane.index)] : 0;
        lane.index++;
        const size_t x = runs[r].at + (o - runs[r].first);  // the value's bytes in the buffer
        if (!runs[r].whole) {
          while (k < arrived.size() && arrived[k].second < x + w) k++;
          if (k == arrived.size() || arrived[k].first > x) {  // not all of them were read
            stats.lost++;
            return;
          }
        }
        uint64_t cur = 0;
        std::memcpy(&cur, data + x, w);  // little-endian host
        if (!matches(*lane.pred, t, cur, prev)) return;
        keep.add(o);
        out.count++;
        if (!out.has_values) return;
        if (out.count > cfg_.value_cap) {
          std::vector<uint64_t>().swap(out.values);
          out.has_values = false;
          return;
        }
        out.values.push_back(cur);
      });
      out.positions.append(keep.finish());
    }
  }
  std::vector<Group> fresh;
  fresh.reserve(lanes.size());
  for (Lane& lane : lanes) {
    lane.out.values.shrink_to_fit();
    fresh.push_back(std::move(lane.out));
  }
  return fresh;
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
