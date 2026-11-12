#include "engine/engine.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <type_traits>

#include "engine/block_scan.hpp"

namespace moth {

namespace {

// A block followed by more of its region is read this much longer, so a value of up to 8 bytes
// starting in its last bytes is seen whole. scan_block's limit keeps it from being found twice.
constexpr size_t kOverlap = 7;

// A narrow reads positions in runs: a run ends at the end of a block, or where the next position,
// of any listed group, is more than this far past the previous one. Each run is read by the calling
// thread and compared by a worker, for every listed group with positions in it.
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

// The readable regions as one step (a scan or a narrow) finds them when it first needs them, after
// a read failed: listed at most once per step, ascending, with touching regions joined. Readable,
// not writable: a page made read-only since the scan can still be read.
class Listing {
 public:
  explicit Listing(ProcessMemory& mem) : mem_(mem) {}

  // The regions that overlap [addr, end), ascending.
  std::span<const Region> overlapping(uint64_t addr, uint64_t end) {
    if (!listed_) list();
    auto from = std::partition_point(regions_.begin(), regions_.end(),
                                     [&](const Region& r) { return r.base + r.size <= addr; });
    auto to = std::partition_point(from, regions_.end(), [&](const Region& r) { return r.base < end; });
    return {from, to};
  }

 private:
  void list() {
    listed_ = true;
    std::vector<Region> rs = mem_.readable_regions();
    std::sort(rs.begin(), rs.end(), [](const Region& a, const Region& b) { return a.base < b.base; });
    for (const Region& r : rs) {
      if (r.size == 0) continue;
      if (!regions_.empty() && r.base <= regions_.back().base + regions_.back().size) {
        Region& last = regions_.back();
        last.size = std::max(last.base + last.size, r.base + r.size) - last.base;
      } else {
        regions_.push_back(r);
      }
    }
  }

  ProcessMemory& mem_;
  bool listed_ = false;
  std::vector<Region> regions_;
};

// Reads n bytes at addr into buf + at and appends the offsets that arrived to spans. Everything
// readable costs one read. A read that comes back short is continued from where it stopped; one
// that gets nothing and covers more than one page is split in two at a page boundary near its
// middle, and each half read the same way. So with a read that gets all or nothing (as
// ReadProcessMemory does), an unreadable page loses only its own bytes, at about two reads per
// halving. Given a listing, such a read first asks it for the readable regions: what lies outside
// all of them (memory the game has freed since it was listed) is not read again, and each part
// inside one is read as above, split only if it fails too. Splitting stops once the process has
// closed. Returns the bytes read.
size_t read_spans(ProcessMemory& mem, uint64_t addr, uint8_t* buf, size_t at, size_t n, Spans& spans,
                  Listing* listing = nullptr) {
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
  if (listing != nullptr) {
    const std::span<const Region> in = listing->overlapping(addr, addr + n);
    if (in.size() != 1 || in[0].base > addr || in[0].base + in[0].size < addr + n) {
      for (const Region& r : in) {  // only the parts still listed
        const uint64_t lo = std::max(addr, r.base), hi = std::min(addr + n, r.base + r.size);
        total += read_spans(mem, lo, buf, at + static_cast<size_t>(lo - addr), static_cast<size_t>(hi - lo), spans);
      }
      return total;
    }
  }
  const uint64_t mid = std::clamp<uint64_t>((addr + n / 2) - (addr + n / 2) % kPage, first, last);
  const size_t left = static_cast<size_t>(mid - addr);
  total += read_spans(mem, addr, buf, at, left, spans);
  return total + read_spans(mem, mid, buf, at + left, n - left, spans);
}

bool compares_with_last(const Pred& p) {
  return p.op == Pred::LtPrev || p.op == Pred::GtPrev || p.op == Pred::EqPrev || p.op == Pred::NePrev ||
         (p.op == Pred::Range && p.moved);
}

void validate(const std::vector<GroupSpec>& specs) {
  for (size_t i = 0; i < specs.size(); i++) {
    const GroupSpec& g = specs[i];
    if (compares_with_last(g.pred))
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

// A buffer that is always written before it is read, so it is never zero-filled.
template <class T> class Buffer {
 public:
  T* data() { return p_.get(); }
  const T* data() const { return p_.get(); }
  // Room for at least n items. Growing does not keep the old contents.
  T* reserve(size_t n) {
    if (n > n_) {
      p_.reset();
      p_.reset(new T[n]);
      n_ = n;
    }
    return p_.get();
  }
  void release() {
    p_.reset();
    n_ = 0;
  }
  size_t capacity() const { return n_; }

 private:
  std::unique_ptr<T[]> p_;
  size_t n_ = 0;
};
using Bytes = Buffer<uint8_t>;

// One block of a scan: read by the calling thread, then searched by a worker.
struct Job {
  uint64_t base = 0;
  uint32_t span = 0;       // the block's own bytes; data holds up to kOverlap more
  Spans parts;             // the bytes of data that were read
  Bytes data;
  std::vector<PositionSet::Block> found;  // per group, set by the worker
  bool done = false;                      // searched
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

// Worker threads that do one step's jobs: a scan's blocks, or a narrow's runs. Only the calling
// thread submits and takes jobs: it submits them in address order and takes them back done, in the
// same order. It submits only while fewer than workers + kQueue jobs are out, so at most kQueue wait
// while every worker holds one. Destruction stops the workers after their current job and joins
// them; an exception a worker throws is rethrown by take. A Job has a bool done.
template <class Job> class Workers {
 public:
  // does(job, w) does a job on worker w, numbered from 0.
  Workers(unsigned workers, std::function<void(Job&, unsigned)> does)
      : does_(std::move(does)), max_out_(workers + kQueue) {
    try {
      for (unsigned w = 0; w < workers; w++) threads_.emplace_back([this, w] { work(w); });
    } catch (...) {
      stop();
      throw;
    }
  }
  ~Workers() { stop(); }
  Workers(const Workers&) = delete;
  Workers& operator=(const Workers&) = delete;

  bool full() const { return out_.size() >= max_out_; }

  // A job to fill, recycled (with its buffers and their room) or new. Call only when not full.
  Job* acquire() {
    if (idle_.empty()) {
      jobs_.push_back(std::make_unique<Job>());
      idle_.push_back(jobs_.back().get());
    }
    Job* j = idle_.back();
    idle_.pop_back();
    return j;
  }
  void release(Job* j) { idle_.push_back(j); }  // a job taken back, or never submitted
  const std::vector<Job*>& idle() const { return idle_; }  // the jobs released and not acquired again

  void submit(Job* j) {
    out_.push_back(j);
    {
      std::lock_guard lock(mu_);
      j->done = false;
      waiting_.push_back(j);
    }
    work_cv_.notify_one();
  }

  // The oldest job out once it is done; null if none is out, or if it is not done yet and wait is
  // false.
  Job* take(bool wait) {
    if (out_.empty()) return nullptr;
    Job* j = out_.front();
    {
      std::unique_lock lock(mu_);
      if (wait) done_cv_.wait(lock, [&] { return j->done || error_; });
      if (error_) std::rethrow_exception(error_);
      if (!j->done) return nullptr;
    }
    out_.pop_front();
    return j;
  }

  // Joins the workers and returns the most that were doing a job at once. Called once every job is
  // taken back.
  unsigned finish() {
    stop();
    return most_busy_;
  }

 private:
  void work(unsigned w) {
    bool busy = false;
    try {
      for (;;) {
        Job* j;
        {
          std::unique_lock lock(mu_);
          work_cv_.wait(lock, [&] { return stopping_ || !waiting_.empty(); });
          if (stopping_) break;
          j = waiting_.front();
          waiting_.pop_front();
          busy = true;
          most_busy_ = std::max(most_busy_, ++busy_);
        }
        does_(*j, w);
        {
          std::lock_guard lock(mu_);
          j->done = true;
          busy = false;
          busy_--;
        }
        done_cv_.notify_one();
      }
    } catch (...) {
      {
        std::lock_guard lock(mu_);
        if (busy) busy_--;
        if (!error_) error_ = std::current_exception();
      }
      done_cv_.notify_one();
    }
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

  const std::function<void(Job&, unsigned)> does_;
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
  unsigned busy_ = 0, most_busy_ = 0;  // workers doing a job now, and the most at once
  std::vector<std::thread> threads_;
};

// The first offset in a block at base whose address is a multiple of align: bit i of a bitmap
// block is the offset first + i * align (see PositionSet::Block).
uint32_t first_aligned(uint32_t align, uint64_t base) { return static_cast<uint32_t>((align - base % align) % align); }

// The places of a snapshot block at base, span bytes long: each multiple of align whose w bytes all
// lie in one stretch that was read. read holds the stretches as offsets in a buffer where base is
// at. Adds them to positions unless it is null, and returns how many there are. When every aligned
// offset of the block is one, which is the case for a block read whole that starts and ends at a
// multiple of 8, the block is added as a full bitmap.
uint64_t add_places(PositionSet* positions, uint32_t align, uint32_t w, uint64_t base, uint32_t span,
                    const Spans& read, size_t at) {
  uint64_t n = 0;
  for (const auto& [from, to] : read) {
    const uint64_t lo = base + (from - at), hi = base + (to - at);
    if (hi < lo + w) continue;
    const uint64_t first = (lo + align - 1) / align, last = (hi - w) / align;
    if (last >= first) n += last - first + 1;
  }
  if (positions == nullptr || n == 0) return n;
  const uint32_t first = first_aligned(align, base);
  if (n == (span > first ? (uint64_t{span} - first + align - 1) / align : 0)) {
    positions->add_full_block(base, span);
    return n;
  }
  PositionSet::Builder b(align, base, span);
  for (const auto& [from, to] : read) {
    const uint64_t lo = base + (from - at), hi = base + (to - at);
    for (uint64_t a = (lo + align - 1) / align * align; a + w <= hi; a += align) b.add(static_cast<uint32_t>(a - base));
  }
  positions->append(b.finish());
  return n;
}

// A stretch of one group's positions in a block, each within kRunGap of the one before.
struct Span {
  uint32_t first, last, end;  // its first and last position, and the end of the last one's value
  size_t lane;                // the group's place in the request
  uint64_t at, n;             // the first position's index among the block's, and how many there are
};

// Appends the spans of a block's positions (offsets aligned to align, values w bytes long) to spans.
// A bitmap is walked a word at a time when no two of a word's positions can be more than kRunGap
// apart, so a dense block costs a step per 32 places, not per position.
void add_spans(const PositionSet::Block& b, uint32_t align, uint32_t w, size_t lane, std::vector<Span>& spans) {
  const size_t from = spans.size();
  uint64_t at = 0;
  // positions lo to hi, n of them, each within kRunGap of the one before
  auto add = [&](uint32_t lo, uint32_t hi, uint64_t n) {
    if (spans.size() == from || lo - spans.back().last > kRunGap) {
      spans.push_back({lo, hi, hi + w, lane, at, n});
    } else {
      spans.back().last = hi;
      spans.back().end = hi + w;
      spans.back().n += n;
    }
    at += n;
  };
  if (!b.bitmap) {
    for (uint32_t o : b.words) add(o, o, 1);
    return;
  }
  const uint32_t first = first_aligned(align, b.base);
  const bool by_word = uint64_t{31} * align <= kRunGap;
  for (size_t k = 0; k < b.words.size(); k++) {
    const uint32_t bits = b.words[k];
    if (bits == 0) continue;
    auto offset = [&](int bit) { return first + static_cast<uint32_t>((k * 32 + static_cast<size_t>(bit)) * align); };
    if (by_word) {
      add(offset(std::countr_zero(bits)), offset(31 - std::countl_zero(bits)), static_cast<uint64_t>(std::popcount(bits)));
      continue;
    }
    for (uint32_t rest = bits; rest != 0; rest &= rest - 1) {
      const uint32_t o = offset(std::countr_zero(rest));
      add(o, o, 1);
    }
  }
}

// One listed group's positions in a run, and what a worker found for them.
struct Part {
  // set by the calling thread
  size_t lane = 0;                            // the group's place in the request
  const PositionSet::Block* block = nullptr;  // the old group's block that holds them
  uint32_t align = 0;
  ValueType type = ValueType::U8;
  const Pred* pred = nullptr;
  unsigned accept = 0;            // a relation: the orders that match (see order); 0: decided by matches()
  const uint8_t* prev = nullptr;  // the old group's stored values, or null when the predicate needs none
  bool from_snapshot = false;     // the predicate takes previous values from the snapshot's bytes
  bool to_snapshot = false;       // the group leans on the snapshot: the bytes of the positions kept replace its own
  uint64_t index = 0;             // the first position's index among the old group's, and its stored value's
  uint64_t at = 0, n = 0;         // the first position's index among the block's, and how many there are
  uint32_t first = 0, last = 0;   // the first and last position
  bool alone = false;             // the group's only part of the block: the worker builds the narrowed block
  bool closes = false;            // the group's last part of the block
  // Otherwise the positions kept go in the run's kept words from kept_at: as a list, or as a bitmap
  // of the places from first to last when that is smaller.
  bool bitmap = false;
  size_t kept_at = 0, kept_words = 0;
  uint64_t copy = 0;              // the values of the first this many positions kept are copied
  size_t values_at = 0;           // into the run's values from here
  // set by the worker
  PositionSet::Block kept_block;  // alone: the positions kept, as the narrowed block
  uint64_t count = 0, lost = 0;   // positions kept, and positions whose bytes were not all read
};

// The snapshot's bytes for the bytes [from, to) of a run's data, saved at p.
struct Held {
  size_t from, to;
  uint8_t* p;
};

// One run of a narrow, read by the calling thread and compared by a worker.
struct RunJob {
  uint64_t base = 0;   // the block's
  uint32_t first = 0;  // the run's first byte, as an offset in the block; data holds the run from it
  Spans arrived;       // the bytes of data that were read
  bool whole = false;  // all of them were
  std::vector<Held> held;   // when a part leans on the snapshot, its stretches that the run overlaps, ascending
  bool held_whole = false;  // one stretch holds all of the run
  Bytes data;
  std::vector<Part> parts;
  Buffer<uint32_t> kept;  // the parts' positions kept, for blocks split across runs
  Bytes values;           // the parts' values copied
  bool done = false;      // compared
};

// Calls f(offset) for each of a part's positions, ascending.
template <class F> void each_position(const Part& p, F&& f) {
  const PositionSet::Block& b = *p.block;
  if (!b.bitmap) {
    for (uint64_t i = p.at; i < p.at + p.n; i++) f(b.words[static_cast<size_t>(i)]);
    return;
  }
  const uint32_t first = first_aligned(p.align, b.base);
  const size_t s0 = (p.first - first) / p.align, s1 = (p.last - first) / p.align;  // their bits
  for (size_t k = s0 / 32; k <= s1 / 32; k++) {
    uint32_t bits = b.words[k];
    if (k == s0 / 32) bits &= ~0u << (s0 % 32);
    if (k == s1 / 32) bits &= ~0u >> (31 - s1 % 32);
    for (; bits != 0; bits &= bits - 1)
      f(first + static_cast<uint32_t>((k * 32 + static_cast<size_t>(std::countr_zero(bits))) * p.align));
  }
}

template <size_t W> uint64_t load(const uint8_t* p) {
  uint64_t v = 0;
  std::memcpy(&v, p, W);  // little-endian host
  return v;
}

// How a value a compares with b, both W bytes in the low bits, as matches() orders them for a
// relation: -1, 0, 1, or 2 when unordered (a NaN). Kind 0: unsigned, 1: signed, 2: a float, which
// compares as matches() compares it as a double, since a float widens to a double exactly.
template <size_t W, int Kind> int order(uint64_t a, uint64_t b) {
  if constexpr (Kind == 2) {
    using F = std::conditional_t<W == 4, float, double>;
    using U = std::conditional_t<W == 4, uint32_t, uint64_t>;
    const F x = std::bit_cast<F>(static_cast<U>(a)), y = std::bit_cast<F>(static_cast<U>(b));
    if (x != x || y != y) return 2;
    return x < y ? -1 : (x > y ? 1 : 0);
  } else if constexpr (Kind == 1) {
    using S = std::conditional_t<W == 1, int8_t, std::conditional_t<W == 2, int16_t, std::conditional_t<W == 4, int32_t, int64_t>>>;
    const S x = static_cast<S>(a), y = static_cast<S>(b);
    return (x > y) - (x < y);
  } else {
    return (a > b) - (a < b);
  }
}

// The orders a relation accepts, a bit for each: bit 0 for -1, 1 for 0, 2 for 1, 3 for unordered.
unsigned accepted_orders(const Pred& p) {
  switch (p.op) {
    case Pred::LtPrev: return 1u << 0;
    case Pred::EqPrev: return 1u << 1;
    case Pred::GtPrev: return 1u << 2;
    case Pred::NePrev: return (1u << 0) | (1u << 2) | (1u << 3);
    default: return 0;
  }
}

// The snapshot's bytes for the W bytes at x in a run's data, or null if they are not all held in
// one stretch. h is the first stretch that ends at or past them; it only moves forward.
template <size_t W> const uint8_t* held_at(const RunJob& j, size_t x, size_t& h) {
  if (j.held_whole) return j.held[0].p + x;
  while (h < j.held.size() && j.held[h].to < x + W) h++;
  if (h == j.held.size() || j.held[h].from > x) return nullptr;
  return j.held[h].p + (x - j.held[h].from);
}

// Compares a part's positions, W bytes each, with their stored values or the snapshot's bytes. A
// position whose bytes were not all read is lost. Kind is the type's for a relation (see order), or -1 for any
// predicate, decided by matches().
template <size_t W, int Kind> void compare(RunJob& j, Part& p) {
  uint8_t* const values = p.copy > 0 ? j.values.data() + p.values_at : nullptr;
  const uint8_t* const data = j.data.data();
  uint64_t i = p.index, count = 0, lost = 0;
  size_t k = 0;  // the first span of arrived that ends at or past the value's bytes; only moves forward
  size_t h = 0;  // the same in held
  // Whether the position at o, the next in order, is kept.
  auto kept = [&](uint32_t o) {
    uint64_t prev = p.prev != nullptr ? load<W>(p.prev + static_cast<size_t>(i) * W) : 0;
    i++;
    const size_t x = o - j.first;  // the value's bytes in data
    if (!j.whole) {
      while (k < j.arrived.size() && j.arrived[k].second < x + W) k++;
      if (k == j.arrived.size() || j.arrived[k].first > x) {  // not all of them were read
        lost++;
        return false;
      }
    }
    if (p.from_snapshot) {
      const uint8_t* const held = held_at<W>(j, x, h);
      if (held == nullptr) {  // never: a snapshot's groups have only places it holds
        lost++;
        return false;
      }
      prev = load<W>(held);
    }
    if constexpr (Kind < 0) {
      if (!matches(*p.pred, p.type, load<W>(data + x), prev)) return false;
    } else {
      if (((p.accept >> (order<W, Kind>(load<W>(data + x), prev) + 1)) & 1) == 0) return false;
    }
    if (count < p.copy) std::memcpy(values + static_cast<size_t>(count) * W, data + x, W);
    count++;
    return true;
  };
  if (p.alone && p.block->bitmap) {
    // A word of places at a time, straight into the narrowed block's bitmap, which has the same
    // places; it becomes the list a Builder would make if that is no larger.
    const PositionSet::Block& b = *p.block;
    const uint32_t first = first_aligned(p.align, b.base);
    std::vector<uint32_t> words(b.words.size(), 0u);
    const size_t s0 = (p.first - first) / p.align, s1 = (p.last - first) / p.align;  // their bits
    for (size_t w = s0 / 32; w <= s1 / 32; w++) {
      uint32_t bits = b.words[w];
      if (w == s0 / 32) bits &= ~0u << (s0 % 32);
      if (w == s1 / 32) bits &= ~0u >> (31 - s1 % 32);
      uint32_t out = 0;
      for (; bits != 0; bits &= bits - 1) {
        const int bit = std::countr_zero(bits);
        if (kept(first + static_cast<uint32_t>((w * 32 + static_cast<size_t>(bit)) * p.align))) out |= 1u << bit;
      }
      words[w] = out;
    }
    p.kept_block = PositionSet::Block{b.base, b.span, static_cast<uint32_t>(count), true, {}};
    if (count > words.size()) {
      p.kept_block.words = std::move(words);
    } else {
      p.kept_block.bitmap = false;
      p.kept_block.words.reserve(static_cast<size_t>(count));
      for (size_t w = 0; w < words.size(); w++)
        for (uint32_t bits = words[w]; bits != 0; bits &= bits - 1)
          p.kept_block.words.push_back(first + static_cast<uint32_t>((w * 32 + static_cast<size_t>(std::countr_zero(bits))) * p.align));
    }
  } else if (p.alone) {
    PositionSet::Builder keep(p.align, p.block->base, p.block->span);
    each_position(p, [&](uint32_t o) {
      if (kept(o)) keep.add(o);
    });
    p.kept_block = keep.finish();
  } else if (p.bitmap) {  // a bit for each place from first to last
    uint32_t* const bits = j.kept.data() + p.kept_at;
    std::fill(bits, bits + p.kept_words, 0u);
    each_position(p, [&](uint32_t o) {
      if (!kept(o)) return;
      const uint32_t place = (o - p.first) / p.align;
      bits[place / 32] |= 1u << (place % 32);
    });
  } else {  // the kept offsets
    uint32_t* const list = j.kept.data() + p.kept_at;
    each_position(p, [&](uint32_t o) {
      if (kept(o)) list[count - 1] = o;
    });
  }
  p.count = count;
  p.lost = lost;
}

// A relation is compared inline for the part's type; any other predicate goes through matches().
template <size_t W> void compare_part(RunJob& j, Part& p) {
  if (p.accept == 0) {
    compare<W, -1>(j, p);
  } else if (W >= 4 && is_float(p.type)) {
    compare<W, 2>(j, p);
  } else if (is_signed(p.type)) {
    compare<W, 1>(j, p);
  } else {
    compare<W, 0>(j, p);
  }
}

// Copies stretches of a run's data into the snapshot's bytes for them, where it holds them. Stretches
// come in ascending order, and touching ones are copied together.
class Replacer {
 public:
  explicit Replacer(RunJob& j) : j_(j) {}
  void add(size_t x, size_t n) {
    if (n_ > 0 && x == x_ + n_) {
      n_ += n;
      return;
    }
    flush();
    x_ = x;
    n_ = n;
  }
  void flush() {
    if (n_ == 0) return;
    const size_t end = x_ + n_;
    while (h_ < j_.held.size() && j_.held[h_].to <= x_) h_++;
    for (size_t k = h_; k < j_.held.size() && j_.held[k].from < end; k++) {
      const Held& held = j_.held[k];
      const size_t lo = std::max(x_, held.from), hi = std::min(end, held.to);
      std::memcpy(held.p + (lo - held.from), j_.data.data() + lo, hi - lo);
    }
    n_ = 0;
  }

 private:
  RunJob& j_;
  size_t x_ = 0, n_ = 0;  // the stretch [x_, x_ + n_) of data, not yet copied
  size_t h_ = 0;          // the first stretch of held that ends after it
};

// Replaces the snapshot's bytes at each position a part kept, W bytes each, with the bytes read.
template <size_t W> void replace_kept(RunJob& j, const Part& p) {
  Replacer r(j);
  // a bitmap of n words whose bit i is the place first + i * align; a full word is one stretch
  // when the places touch
  auto bitmap = [&](const uint32_t* words, size_t n, uint32_t first) {
    for (size_t k = 0; k < n; k++) {
      const uint32_t o = first + static_cast<uint32_t>(k * 32 * p.align);
      if (words[k] == ~0u && p.align == W) {
        r.add(o - j.first, 32 * W);
        continue;
      }
      for (uint32_t bits = words[k]; bits != 0; bits &= bits - 1)
        r.add(o + static_cast<uint32_t>(std::countr_zero(bits)) * p.align - j.first, W);
    }
  };
  if (p.alone && p.kept_block.bitmap) {
    bitmap(p.kept_block.words.data(), p.kept_block.words.size(), first_aligned(p.align, p.kept_block.base));
  } else if (p.alone) {
    for (uint32_t o : p.kept_block.words) r.add(o - j.first, W);
  } else if (p.bitmap) {
    bitmap(j.kept.data() + p.kept_at, p.kept_words, p.first);
  } else {
    for (uint64_t c = 0; c < p.count; c++) r.add(j.kept.data()[p.kept_at + static_cast<size_t>(c)] - j.first, W);
  }
  r.flush();
}

// A worker's share of a narrow: one run, for each group with positions in it. The snapshot's bytes
// are replaced only once every part is compared, since a place one part kept may be the bytes
// another part compares with (an i32 place holds four u8 places).
void compare_run(RunJob& j) {
  for (Part& p : j.parts) {
    switch (width_of(p.type)) {
      case 1: compare_part<1>(j, p); break;
      case 2: compare_part<2>(j, p); break;
      case 4: compare_part<4>(j, p); break;
      default: compare_part<8>(j, p); break;
    }
  }
  for (const Part& p : j.parts) {
    if (!p.to_snapshot || p.count == 0) continue;
    switch (width_of(p.type)) {
      case 1: replace_kept<1>(j, p); break;
      case 2: replace_kept<2>(j, p); break;
      case 4: replace_kept<4>(j, p); break;
      default: replace_kept<8>(j, p); break;
    }
  }
}

// Copies one value of w bytes (1, 2, 4 or 8). Each width is a copy of a fixed size, which is a
// plain load and store; a copy of a variable size would be a call.
void copy_value(void* to, const void* from, size_t w) {
  switch (w) {
    case 1: std::memcpy(to, from, 1); return;
    case 2: std::memcpy(to, from, 2); return;
    case 4: std::memcpy(to, from, 4); return;
    default: std::memcpy(to, from, 8); return;
  }
}

// A share of a group's blocks whose values a worker takes from the snapshot, and where they go.
struct TakeJob {
  std::span<const PositionSet::Block> blocks;
  uint8_t* out = nullptr;
  bool held = true;   // false if a position's bytes were not held
  bool done = false;  // taken
};

// A share is at least this many values, so a small group takes its values on the calling thread.
constexpr uint64_t kTakeShare = uint64_t{1} << 20;

// Copies the snapshot's bytes at each position of a share's blocks (offsets aligned to align), W
// bytes each, to its out, in order. A bitmap word of places that touch is one copy.
template <size_t W> void take_values(const Snapshot& snapshot, TakeJob& t, uint32_t align) {
  uint8_t* out = t.out;
  for (const PositionSet::Block& b : t.blocks) {
    const uint8_t* const whole = snapshot.at(b.base, b.span);  // a block read whole is one stretch
    auto one = [&](uint32_t o) {
      const uint8_t* const p = whole != nullptr ? whole + o : snapshot.at(b.base + o, W);
      if (p == nullptr) return false;  // never: a snapshot's groups have only places it holds
      std::memcpy(out, p, W);
      out += W;
      return true;
    };
    if (!b.bitmap) {
      for (uint32_t o : b.words)
        if (!one(o)) t.held = false;
      continue;
    }
    const uint32_t first = first_aligned(align, b.base);
    for (size_t k = 0; k < b.words.size(); k++) {
      const uint32_t o = first + static_cast<uint32_t>(k * 32 * align);
      if (b.words[k] == ~0u && align == W && whole != nullptr) {
        std::memcpy(out, whole + o, 32 * W);
        out += 32 * W;
        continue;
      }
      for (uint32_t bits = b.words[k]; bits != 0; bits &= bits - 1)
        if (!one(o + static_cast<uint32_t>(std::countr_zero(bits)) * align)) t.held = false;
    }
  }
}

}  // namespace

Engine::Values& Engine::Values::operator=(Values&& o) noexcept {
  if (this == &o) return *this;
  width_ = o.width_;
  used_ = std::exchange(o.used_, 0);
  bytes_ = std::move(o.bytes_);
  o.bytes_.clear();
  return *this;
}

uint64_t Engine::Values::get(uint64_t i) const {
  uint64_t bits = 0;
  copy_value(&bits, bytes_.data() + static_cast<size_t>(i) * width_, width_);  // little-endian host
  return bits;
}

void Engine::Values::set(uint64_t i, uint64_t bits) {
  copy_value(bytes_.data() + static_cast<size_t>(i) * width_, &bits, width_);  // little-endian host
}

// Growing is rare (the room doubles), and kept out of line so an append stays a few instructions.
void Engine::Values::append(const uint8_t* p) {
  if (bytes_.size() - used_ < width_) [[unlikely]] grow(width_);
  copy_value(bytes_.data() + used_, p, width_);
  used_ += width_;
}

void Engine::Values::append(const uint8_t* p, uint64_t n) {
  const size_t len = static_cast<size_t>(n) * width_;
  if (len == 0) return;
  if (bytes_.size() - used_ < len) grow(len);
  std::memcpy(bytes_.data() + used_, p, len);
  used_ += len;
}

void Engine::Values::grow(size_t need) { bytes_.resize(std::max(2 * bytes_.size(), used_ + need)); }

uint8_t* Engine::Values::extend(uint64_t n) {
  const size_t len = static_cast<size_t>(n) * width_;
  if (bytes_.size() - used_ < len) bytes_.resize(used_ + len);
  used_ += len;
  return bytes_.data() + (used_ - len);
}

void Engine::Values::clear() {
  std::vector<uint8_t>().swap(bytes_);
  used_ = 0;
}

void Engine::Values::shrink_to_fit() {
  bytes_.resize(used_);
  bytes_.shrink_to_fit();
}

Engine::Engine(ProcessMemory& mem, EngineConfig cfg) : mem_(mem), cfg_(cfg), workers_(resolve_workers(cfg.workers)) {
  if (cfg_.block_size == 0 || cfg_.block_size > (size_t{1} << 30))
    throw EngineError("bad_request", "block_size must be between 1 byte and 1 GiB");
}

std::vector<GroupInfo> Engine::scan(const std::vector<GroupSpec>& specs, bool keep, ScanStats& stats) {
  const auto start = std::chrono::steady_clock::now();
  stats = ScanStats{};
  validate(specs);
  if (!mem_.alive()) throw EngineError("target_gone", "the game has closed");

  // Free the groups being replaced first, so old and new candidates are never held together, and
  // the snapshot if no group is left leaning on it.
  if (keep)
    std::erase_if(groups_, [&](const Group& g) { return listed(specs, g.spec.id); });
  else
    groups_.clear();
  release_unused_snapshot();
  const uint64_t kept = storage_bytes();
  std::vector<Group> fresh;
  fresh.reserve(specs.size());
  for (const GroupSpec& s : specs) fresh.emplace_back(s);

  std::vector<Region> regions = mem_.writable_regions();
  std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) { return a.base < b.base; });
  Listing listing(mem_);  // the readable regions, listed once if a read fails during the walk

  peak_raw_offsets_ = 0;
  peak_walk_bytes_ = 0;
  // each worker's scratch for one slice's raw offsets, per group: it only grows while the walk runs
  std::vector<std::vector<std::vector<uint32_t>>> hits(workers_, std::vector<std::vector<uint32_t>>(specs.size()));
  Workers<Job> searchers(workers_, [&](Job& j, unsigned w) { search(j, specs, hits[w]); });
  // Adds the oldest searched block's matches to the groups, in address order whatever the
  // worker count. Returns false when no block is out, or wait is false and it is not searched yet.
  auto add_next = [&](bool wait) {
    Job* j = searchers.take(wait);
    if (j == nullptr) return false;
    for (size_t i = 0; i < fresh.size(); i++) add_found(fresh[i], std::move(j->found[i]), j->data.data());
    // Holding the storage within the budget after every block keeps memory near it during the
    // walk instead of only after it. Untracking here depends on positions alone, which only grow,
    // so the end would untrack the same groups. Values dropped here could have fit at the end when
    // storage counted beside them is freed later: a group passing value_cap frees its values, an
    // untracked group its positions. The budget counts the values held, not the room their
    // buffers keep to grow into, so memory can run over it by up to the values held until the
    // walk ends.
    fit_budget(fresh, kept);
    uint64_t counted = 0;
    for (const Group& g : fresh) counted += g.positions.bytes() + g.values.used_bytes();
    peak_walk_bytes_ = std::max(peak_walk_bytes_, counted);
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
      uint8_t* data = j->data.reserve(cfg_.block_size + kOverlap);
      read_spans(mem_, j->base, data, 0, want, j->parts, &listing);  // only what arrived is searched
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
  searchers.finish();
  // the raw offsets the workers had room for, which is the most they held at once
  for (const auto& per_group : hits)
    for (const std::vector<uint32_t>& h : per_group) peak_raw_offsets_ += h.capacity();
  if (!mem_.alive()) throw EngineError("target_gone", "the game has closed");
  for (Group& g : fresh) g.values.shrink_to_fit();  // the room to grow into is no longer needed
  fit_budget(fresh, kept);  // decides as after the last block, so it acts only if no block was walked

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
  if (g.has_values && g.count > cfg_.value_cap) forget_values(g);
  if (g.has_values) g.positions.for_each_in(b, [&](uint32_t o) { g.values.append(data + o); });
  g.positions.append(std::move(b));
}

// Holds kept plus the fresh groups' storage within the budget, counting each fresh group's
// positions and the values it holds (not its value buffer's room to grow). First untracks fresh
// groups from the last one backwards until the positions alone fit; then drops the values of
// tracked fresh groups from the last one backwards while positions and values are still over,
// each group staying tracked. No group is untracked to make room for values.
void Engine::fit_budget(std::vector<Group>& fresh, uint64_t kept) {
  uint64_t positions = kept, values = 0;
  for (const Group& g : fresh) {
    positions += g.positions.bytes();
    values += g.values.used_bytes();
  }
  for (size_t i = fresh.size(); i-- > 0 && positions > cfg_.budget_bytes;) {
    if (!fresh[i].tracked) continue;
    positions -= fresh[i].positions.bytes();
    values -= fresh[i].values.used_bytes();
    untrack(fresh[i]);
  }
  for (size_t i = fresh.size(); i-- > 0 && positions + values > cfg_.budget_bytes;) {
    if (!fresh[i].has_values) continue;
    values -= fresh[i].values.used_bytes();
    forget_values(fresh[i]);
  }
}

std::vector<GroupInfo> Engine::snapshot(const std::vector<SnapSpec>& specs, ScanStats& stats) {
  const auto start = std::chrono::steady_clock::now();
  stats = ScanStats{};
  for (size_t i = 0; i < specs.size(); i++) {
    const SnapSpec& s = specs[i];
    if (s.align == 0 || s.align % width_of(s.type) != 0)
      throw EngineError("bad_request", "a snapshot group's align must be a positive multiple of its type's width: " + s.id);
    for (size_t j = 0; j < i; j++)
      if (specs[j].id == s.id) throw EngineError("bad_request", "group listed twice: " + s.id);
  }
  if (!mem_.alive()) throw EngineError("target_gone", "the game has closed");
  std::vector<Region> regions = mem_.writable_regions();
  std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) { return a.base < b.base; });
  uint64_t total = 0;
  for (const Region& r : regions) total += r.size;
  if (total > cfg_.snapshot_cap_bytes)
    throw EngineError("too_big", std::to_string(total) + " to snapshot, limit " + std::to_string(cfg_.snapshot_cap_bytes));

  // Release first, so two snapshots, or a snapshot and the candidates it replaces, are never held
  // together.
  groups_.clear();
  snapshot_.clear();
  std::vector<Group> fresh;
  try {
    fresh = copy_regions(regions, specs, stats);
    if (!mem_.alive()) throw EngineError("target_gone", "the game has closed");
  } catch (const std::bad_alloc&) {  // the groups' storage is freed by now
    snapshot_.clear();
    throw EngineError("too_big", "could not get memory for the snapshot");
  } catch (...) {
    snapshot_.clear();
    throw;
  }
  std::vector<GroupInfo> out;
  out.reserve(fresh.size());
  for (Group& g : fresh) {
    g.snap = g.tracked;
    out.push_back(info(g));
    groups_.push_back(std::move(g));
  }
  release_unused_snapshot();
  stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return out;
}

// Reads each region, block by block, into a buffer of the snapshot's own, and returns the listed
// groups with the places of their type in the bytes that arrived, untracked from the last one
// backwards after each block while their positions are over the budget. A group gets none of its
// values: they are the snapshot's.
std::vector<Engine::Group> Engine::copy_regions(const std::vector<Region>& regions, const std::vector<SnapSpec>& specs,
                                                ScanStats& stats) {
  std::vector<Group> fresh;
  fresh.reserve(specs.size());
  for (const SnapSpec& s : specs) {
    fresh.emplace_back(GroupSpec{s.id, s.type, s.align, 0, Pred{Pred::EqPrev}});
    fresh.back().has_values = false;
  }
  Listing listing(mem_);  // the readable regions, listed once if a read fails
  // A block that starts at a multiple of 8 then ends at one, so no aligned place runs over its end.
  const uint64_t step = std::max<uint64_t>(8, cfg_.block_size / 8 * 8);
  Spans read, held;
  for (const Region& r : regions) {
    stats.regions++;
    uint8_t* const buffer = snapshot_.add_region(r.size);
    held.clear();
    for (uint64_t off = 0; off < r.size; off += step) {
      const uint32_t span = static_cast<uint32_t>(std::min(step, r.size - off));
      read.clear();
      const size_t got = read_spans(mem_, r.base + off, buffer, static_cast<size_t>(off), span, read, &listing);
      stats.bytes += got;
      stats.unreadable_bytes += span - got;
      for (const auto& [from, to] : read) add_span(held, from, to);
      if (got == 0) continue;
      for (Group& g : fresh)
        g.count += add_places(g.tracked ? &g.positions : nullptr, g.spec.align,
                              static_cast<uint32_t>(width_of(g.spec.type)), r.base + off, span, read, static_cast<size_t>(off));
      fit_budget(fresh, 0);
    }
    for (const auto& [from, to] : held) snapshot_.hold(r.base + from, to - from, buffer + from);
    snapshot_.end_region();
  }
  return fresh;
}

std::vector<Item> Engine::peek(const std::string& id, uint64_t offset, uint64_t limit, bool holes) {
  const Group& g = find(id);
  if (!mem_.alive()) throw EngineError("target_gone", "the game has closed");
  std::vector<Item> items;
  if (!g.tracked) return items;
  const size_t w = width_of(g.spec.type);
  const std::vector<uint64_t> addrs = g.positions.addresses(offset, limit);
  bool missed = false;
  for (uint64_t addr : addrs) {
    uint64_t bits = 0;
    if (mem_.read(addr, &bits, w) == w) {
      items.push_back({addr, bits});  // little-endian host
    } else {
      missed = true;
      if (holes) items.push_back({addr, 0, false});
    }
  }
  // reads also fail once the game closes; that is reported, not taken as freed memory
  if (missed && !mem_.alive()) throw EngineError("target_gone", "the game has closed");
  return items;
}

std::vector<Item> Engine::peek_from(const std::string& id, uint64_t addr, uint64_t limit, bool holes) {
  return peek(id, find(id).positions.lower_bound(addr), limit, holes);
}

bool Engine::narrow(const std::vector<NarrowItem>& items, bool atomic, std::vector<GroupInfo>& out,
                    ScanStats& stats) {
  const auto start = std::chrono::steady_clock::now();
  stats = ScanStats{};
  narrow_threads_used_ = 0;
  peak_narrow_held_bytes_ = 0;
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
    if (compares_with_last(it.pred) && !g.has_values && !g.snap)
      throw EngineError("no_values", "group " + it.id + " has no stored values");
    at.push_back(k);
  }
  if (!mem_.alive()) throw EngineError("target_gone", "the game has closed");

  // The replacements are built beside the groups, so nothing changes until the narrow applies,
  // but the snapshot: its bytes are replaced in place at the places kept, so a step that keeps
  // nothing replaces nothing. A step that fails after that releases it.
  std::vector<Group> fresh;
  bool replacing = false;  // a run leaning on the snapshot was handed out
  try {
    fresh = narrowed(at, items, stats, replacing);
    // Reads also fail once the game closes; that is reported, not taken as lost matches.
    if (!mem_.alive()) throw EngineError("target_gone", "the game has closed");
  } catch (...) {
    if (replacing) forget_snapshot();
    throw;
  }
  uint64_t total = 0;
  for (const Group& g : fresh) total += g.count;

  const bool applied = !atomic || total > 0;
  if (applied) {
    fit_gained_values(fresh, at);
    for (size_t i = 0; i < fresh.size(); i++) groups_[at[i]] = std::move(fresh[i]);
    release_unused_snapshot();
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
// The calling thread finds the runs and reads them, in address order; a worker compares one run
// for every group with positions in it; the calling thread then adds what it kept to the groups,
// run by run in the same order, so they are built exactly as one thread would build them. A
// group's block that lies in one run is built whole by the worker; one that spans several is put
// together from their kept positions.
// Beyond its own bytes, a run needs room for the positions kept in blocks split across runs and for
// the values its worker copies. That room is set aside before the run is handed out, and kept for
// the next run when it comes back, and all of it stays within (workers + kQueue) * block_size,
// unless one run's kept positions alone need more. A worker copies the values of the positions it
// keeps only within its group's room under value_cap, less what runs already out may copy, and
// within that allowance; the calling thread takes any other value it needs from the run's bytes
// when it takes the run back. The narrowed blocks the workers build are the groups' new positions,
// at most a bit per place, and are not counted.
std::vector<Engine::Group> Engine::narrowed(const std::vector<size_t>& at, const std::vector<NarrowItem>& items,
                                            ScanStats& stats, bool& replacing) {
  struct Lane {
    const Group* old;
    const Pred* pred;
    Group out;
    size_t block = 0;    // the next of old's blocks to narrow
    uint64_t index = 0;  // of that block's first position in old, which is also its stored value's index
    std::optional<PositionSet::Builder> pending = std::nullopt;  // a narrowed block put together from several runs
    uint64_t promised = 0;  // the values runs out may copy
  };
  // Positions first..last of a block (offsets), whose values take bytes [first, end): the spans
  // [from, to) of the block's spans, in address order.
  struct Run {
    uint32_t first, last, end;
    size_t from, to;
  };
  constexpr size_t kNone = ~size_t{0};
  std::vector<Lane> lanes;
  lanes.reserve(at.size());
  for (size_t i = 0; i < at.size(); i++) {
    lanes.push_back({&groups_[at[i]], &items[i].pred, Group(groups_[at[i]].spec)});
    if (lanes.back().old->snap) {  // leans on the snapshot until fit_gained_values gives it values
      lanes.back().out.has_values = false;
      lanes.back().out.snap = true;
    }
  }
  std::vector<size_t> set;  // the lanes narrowing the current block
  std::vector<Span> spans;
  std::vector<Run> runs;
  // per lane, for the current block: its first and last run, and its part in the run being filled
  std::vector<size_t> first_run(lanes.size()), last_run(lanes.size()), part(lanes.size());
  Listing listing(mem_);    // the readable regions, listed once if a read fails
  const uint64_t allowance = (uint64_t{workers_} + kQueue) * cfg_.block_size;
  uint64_t held = 0;        // the room the runs have, out or back, beyond their own bytes

  // After lanes, so the workers stop before the groups their runs point into go away.
  Workers<RunJob> comparers(workers_, [](RunJob& j, unsigned) { compare_run(j); });
  // Adds the oldest compared run to the narrowed groups. Returns false when no run is out, or wait
  // is false and it is not compared yet.
  auto add_next = [&](bool wait) {
    RunJob* j = comparers.take(wait);
    if (j == nullptr) return false;
    for (Part& p : j->parts) {
      Lane& lane = lanes[p.lane];
      Group& out = lane.out;
      stats.lost += p.lost;
      lane.promised -= p.copy;
      // As one position at a time would: values go once the count passes value_cap.
      out.count += p.count;
      if (out.has_values && out.count > cfg_.value_cap) forget_values(out);
      // The values the worker copied, then the rest from the run's bytes, in position order.
      const uint64_t copied = std::min(p.count, p.copy);
      if (out.has_values) out.values.append(j->values.data() + p.values_at, copied);
      const bool fill = out.has_values && copied < p.count;
      uint64_t k = 0;
      auto kept = [&](uint32_t o) {
        if (fill && k >= copied) out.values.append(j->data.data() + (o - j->first));
        k++;
      };
      if (p.alone) {
        if (fill) out.positions.for_each_in(p.kept_block, kept);
        out.positions.append(std::move(p.kept_block));
        continue;
      }
      if (!lane.pending) lane.pending.emplace(p.align, p.block->base, p.block->span);
      const uint32_t* words = j->kept.data() + p.kept_at;
      auto add = [&](uint32_t o) {
        lane.pending->add(o);
        kept(o);
      };
      if (!p.bitmap) {
        for (uint64_t i = 0; i < p.count; i++) add(words[i]);
      } else {
        for (size_t w = 0; w < p.kept_words; w++)
          for (uint32_t bits = words[w]; bits != 0; bits &= bits - 1)
            add(p.first + static_cast<uint32_t>((w * 32 + static_cast<size_t>(std::countr_zero(bits))) * p.align));
      }
      if (p.closes) {
        out.positions.append(lane.pending->finish());
        lane.pending.reset();
      }
    }
    if (held > allowance) {  // only after one run's kept positions alone needed more
      held -= j->values.capacity() + 4 * uint64_t{j->kept.capacity()};
      j->values.release();
      j->kept.release();
    }
    comparers.release(j);
    return true;
  };

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

    // Each lane's own spans, then merged into runs: two join when one starts within kRunGap of the
    // other's last position, which is the run rule applied to all the lanes' positions together.
    spans.clear();
    for (size_t i : set) {
      const Group& g = *lanes[i].old;
      add_spans(g.positions.blocks()[lanes[i].block], g.spec.align, static_cast<uint32_t>(width_of(g.spec.type)), i,
                spans);
      first_run[i] = kNone;
    }
    if (set.size() > 1) std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.first < b.first; });
    runs.clear();
    for (size_t s = 0; s < spans.size(); s++) {
      const Span& sp = spans[s];
      if (!runs.empty() && sp.first <= runs.back().last + kRunGap) {
        runs.back().last = std::max(runs.back().last, sp.last);
        runs.back().end = std::max(runs.back().end, sp.end);
        runs.back().to = s + 1;
      } else {
        runs.push_back({sp.first, sp.last, sp.end, s, s + 1});
      }
      if (first_run[sp.lane] == kNone) first_run[sp.lane] = runs.size() - 1;
      last_run[sp.lane] = runs.size() - 1;
    }

    // Each run read (with one call unless part of it is unreadable) into a job of its own, and
    // handed to the workers with each lane's part of it. A lane's spans in one run are next to
    // each other in its own order, so its part is one stretch of its block's positions.
    for (size_t r = 0; r < runs.size(); r++) {
      while (comparers.full()) add_next(true);
      RunJob* j = comparers.acquire();
      j->base = base;
      j->first = runs[r].first;
      size_t parts = 0;
      for (size_t i : set) part[i] = kNone;
      for (size_t s = runs[r].from; s < runs[r].to; s++) {
        const Span& sp = spans[s];
        if (part[sp.lane] == kNone) {  // the lane's first span in this run
          if (j->parts.size() == parts) j->parts.emplace_back();
          Part& p = j->parts[parts];
          const Lane& lane = lanes[sp.lane];
          const Group& g = *lane.old;
          part[sp.lane] = parts++;
          p.lane = sp.lane;
          p.block = &g.positions.blocks()[lane.block];
          p.align = g.spec.align;
          p.type = g.spec.type;
          p.pred = lane.pred;
          p.accept = accepted_orders(*lane.pred);
          p.prev = g.has_values && compares_with_last(*lane.pred) ? g.values.data() : nullptr;
          p.from_snapshot = g.snap && compares_with_last(*lane.pred);
          p.to_snapshot = g.snap;
          p.index = lane.index + sp.at;
          p.at = sp.at;
          p.n = 0;
          p.first = sp.first;
          p.alone = first_run[sp.lane] == last_run[sp.lane];
          p.closes = last_run[sp.lane] == r;
        }
        Part& p = j->parts[part[sp.lane]];
        p.last = sp.last;
        p.n += sp.n;
      }
      j->parts.resize(parts);

      // Room for the positions kept in a split block: a list, or a bitmap of the places from the
      // part's first position to its last, whichever is smaller.
      size_t kept_words = 0;
      for (Part& p : j->parts) {
        if (p.alone) continue;
        const uint64_t bitmap_words = ((p.last - p.first) / p.align + 1 + 31) / 32;
        p.bitmap = p.n > bitmap_words;
        p.kept_at = kept_words;
        p.kept_words = static_cast<size_t>(p.bitmap ? bitmap_words : p.n);
        kept_words += p.kept_words;
      }
      // what growing this run's room to hold the kept positions and v bytes of values adds
      auto growth = [&](uint64_t v) {
        return (v > j->values.capacity() ? v - j->values.capacity() : 0) +
               4 * uint64_t{kept_words > j->kept.capacity() ? kept_words - j->kept.capacity() : 0};
      };
      // Room for the values copied: within each group's room under value_cap, less what runs out
      // may copy. While the allowance is short, the room of runs back from the workers is freed,
      // then runs out are waited for; after that fewer values are copied, and this run's own room
      // for values gives way to its kept positions.
      for (;;) {
        uint64_t want = 0;
        for (Part& p : j->parts) {
          const Lane& lane = lanes[p.lane];
          const uint64_t used = std::min(cfg_.value_cap, lane.out.count + lane.promised);
          p.copy = lane.out.has_values ? std::min(p.n, cfg_.value_cap - used) : 0;
          want += p.copy * width_of(p.type);
        }
        for (RunJob* k : comparers.idle()) {
          if (held + growth(want) <= allowance) break;
          held -= k->values.capacity() + 4 * uint64_t{k->kept.capacity()};
          k->values.release();
          k->kept.release();
        }
        if (held + growth(want) <= allowance || !add_next(true)) break;
      }
      if (held + growth(0) > allowance) {
        held -= j->values.capacity();
        j->values.release();
      }
      uint64_t left = j->values.capacity() + allowance - std::min(allowance, held + growth(0));
      size_t values_at = 0;
      for (Part& p : j->parts) {
        const uint64_t w = width_of(p.type);
        p.copy = std::min(p.copy, left / w);
        left -= p.copy * w;
        p.values_at = values_at;
        values_at += static_cast<size_t>(p.copy * w);
        lanes[p.lane].promised += p.copy;
      }
      held += growth(values_at);
      if (kept_words > 0) j->kept.reserve(kept_words);
      if (values_at > 0) j->values.reserve(values_at);
      peak_narrow_held_bytes_ = std::max(peak_narrow_held_bytes_, held);

      const size_t n = runs[r].end - runs[r].first;
      // The snapshot's bytes for the run, when a part leans on it: the run's worker alone reads and
      // replaces them, since runs do not overlap and a snapshot group's places lie within a block.
      j->held.clear();
      if (std::any_of(j->parts.begin(), j->parts.end(), [](const Part& p) { return p.to_snapshot; })) {
        const uint64_t lo = base + runs[r].first, hi = lo + n;
        snapshot_.each_overlapping(lo, hi, [&](const Snapshot::Piece& piece) {
          const uint64_t from = std::max(lo, piece.base), to = std::min(hi, piece.base + piece.size);
          j->held.push_back({static_cast<size_t>(from - lo), static_cast<size_t>(to - lo), piece.data + (from - piece.base)});
        });
      }
      j->held_whole = j->held.size() == 1 && j->held[0].from == 0 && j->held[0].to == n;
      j->arrived.clear();
      const size_t got = read_spans(mem_, base + runs[r].first, j->data.reserve(n), 0, n, j->arrived, &listing);
      j->whole = got == n;
      stats.bytes += got;
      if (!j->held.empty()) replacing = true;
      comparers.submit(j);
      while (add_next(false)) {
      }
    }
    for (size_t i : set) {
      lanes[i].index += lanes[i].old->positions.blocks()[lanes[i].block].count;
      lanes[i].block++;
    }
  }
  while (add_next(true)) {
  }
  narrow_threads_used_ = comparers.finish();
  std::vector<Group> fresh;
  fresh.reserve(lanes.size());
  for (Lane& lane : lanes) {
    lane.out.values.shrink_to_fit();
    fresh.push_back(std::move(lane.out));
  }
  return fresh;
}

// Values a narrowed group gains (it had none, and is now within value_cap) are kept, in request
// order, only while all storage stays within budget_bytes. A group that leans on the snapshot gains
// them from it, which holds the bytes just read at every place the group kept; without them it
// goes on leaning on the snapshot. Values a group already had only shrink.
void Engine::fit_gained_values(std::vector<Group>& fresh, const std::vector<size_t>& at) {
  auto from_snapshot = [&](size_t i) { return fresh[i].snap && fresh[i].count <= cfg_.value_cap; };
  auto gains = [&](size_t i) { return from_snapshot(i) || (fresh[i].has_values && !groups_[at[i]].has_values); };
  uint64_t total = storage_bytes();
  for (size_t i = 0; i < fresh.size(); i++)
    total = total - bytes_of(groups_[at[i]]) + (gains(i) ? fresh[i].positions.bytes() : bytes_of(fresh[i]));
  for (size_t i = 0; i < fresh.size(); i++) {
    if (!gains(i)) continue;
    const bool snap = from_snapshot(i);
    const uint64_t v = snap ? fresh[i].count * width_of(fresh[i].spec.type) : fresh[i].values.bytes();
    if (total + v <= cfg_.budget_bytes && (!snap || take_values_from_snapshot(fresh[i]))) {
      total += v;
      continue;
    }
    if (!snap) forget_values(fresh[i]);
  }
}

// Gives a group that leans on the snapshot its values: the snapshot's bytes at its positions. A
// large group's blocks are shared out among workers_used() threads, each copying into its own part
// of the values. Returns false, the group still leaning on the snapshot, if there is no memory for
// them or no thread could be started.
bool Engine::take_values_from_snapshot(Group& g) {
  const std::vector<PositionSet::Block>& blocks = g.positions.blocks();
  bool held = true;
  try {
    uint8_t* const out = g.values.extend(g.count);
    const size_t w = width_of(g.spec.type);
    auto take = [&](TakeJob& t) {
      switch (w) {
        case 1: take_values<1>(snapshot_, t, g.spec.align); break;
        case 2: take_values<2>(snapshot_, t, g.spec.align); break;
        case 4: take_values<4>(snapshot_, t, g.spec.align); break;
        default: take_values<8>(snapshot_, t, g.spec.align); break;
      }
    };
    std::optional<Workers<TakeJob>> takers;
    if (workers_ > 1 && g.count > kTakeShare) takers.emplace(workers_, [&](TakeJob& t, unsigned) { take(t); });
    auto collect = [&](bool wait) {
      TakeJob* t = takers->take(wait);
      if (t == nullptr) return false;
      held = held && t->held;
      takers->release(t);
      return true;
    };
    uint64_t index = 0;  // of the share's first value
    for (size_t b = 0; b < blocks.size();) {
      TakeJob local;
      if (takers)
        while (takers->full()) collect(true);
      TakeJob& t = takers ? *takers->acquire() : local;
      t.blocks = std::span(blocks).subspan(b);
      t.out = out + static_cast<size_t>(index) * w;
      t.held = true;
      uint64_t n = 0;
      size_t e = b;
      while (e < blocks.size() && n < kTakeShare) n += blocks[e++].count;
      t.blocks = t.blocks.first(e - b);
      b = e;
      index += n;
      if (!takers) {
        take(t);
        held = held && t.held;
        continue;
      }
      takers->submit(&t);
      while (collect(false)) {
      }
    }
    if (takers) {
      while (collect(true)) {
      }
      takers->finish();
    }
  } catch (...) {  // no memory, or no thread
    held = false;
  }
  if (!held) {
    g.values.clear();
    return false;
  }
  g.has_values = true;
  g.snap = false;
  return true;
}

void Engine::forget_snapshot() {
  snapshot_.clear();
  for (Group& g : groups_) g.snap = false;
}

void Engine::release_unused_snapshot() {
  if (std::none_of(groups_.begin(), groups_.end(), [](const Group& g) { return g.snap; })) snapshot_.clear();
}

void Engine::drop(const std::vector<std::string>& ids) {
  for (const std::string& id : ids) find(id);  // throws for an unknown id before anything is removed
  std::erase_if(groups_, [&](const Group& g) { return std::find(ids.begin(), ids.end(), g.spec.id) != ids.end(); });
  release_unused_snapshot();
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
  const size_t w = width_of(type);
  if (!mem_.write(addr, &bits, w)) return false;  // the low bytes, little-endian host
  prev_bits = prev;
  // what was written is now the last value seen there, for every group that reads those bytes
  for (Group& g : groups_) {
    uint64_t i = 0;
    if (g.has_values && width_of(g.spec.type) == w && g.positions.index_of(addr, i)) g.values.set(i, bits);
  }
  snapshot_.put(addr, &bits, w);  // little-endian host
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

GroupInfo Engine::info(const Group& g) { return {g.spec.id, g.count, g.tracked, g.has_values || g.snap}; }

uint64_t Engine::bytes_of(const Group& g) { return g.positions.bytes() + g.values.bytes(); }

void Engine::untrack(Group& g) {
  g.positions = PositionSet(g.spec.align);
  forget_values(g);
  g.tracked = false;
  g.snap = false;
}

void Engine::forget_values(Group& g) {
  g.values.clear();
  g.has_values = false;
}

size_t Engine::index_of(const std::string& id) const {
  for (size_t i = 0; i < groups_.size(); i++)
    if (groups_[i].spec.id == id) return i;
  throw EngineError("no_such_group", "no group named " + id);
}

const Engine::Group& Engine::find(const std::string& id) const { return groups_[index_of(id)]; }

}  // namespace moth
