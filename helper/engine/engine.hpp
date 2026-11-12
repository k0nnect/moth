#pragma once
// Engine: walks a process's writable memory in blocks and keeps, per group, what matched.
// Portable C++20, no OS calls.
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "engine/memory.hpp"
#include "engine/posset.hpp"
#include "engine/pred.hpp"

namespace moth {

// tracked: the group's positions are stored (otherwise only its count is).
// has_values: its last-seen values are stored too, which happens only while count <= value_cap
// and the budget has room for them (see Engine::scan and Engine::narrow).
struct GroupInfo {
  std::string id;
  uint64_t count;
  bool tracked;
  bool has_values;
};

// regions: writable regions walked. bytes: bytes read and searched. unreadable_bytes: bytes in
// those regions that could not be read. lost: candidates dropped because their memory went away.
// ms: wall-clock time of the step. A narrow walks no regions: its regions and unreadable_bytes are 0.
// A read that fails or comes back short is read again in smaller parts, down to single pages, so
// only bytes that really cannot be read are left out (see read_spans in engine.cpp). After a failed
// read a step lists the readable regions once, and bytes no longer in any are left out unread.
struct ScanStats {
  uint64_t regions = 0, bytes = 0, unreadable_bytes = 0, lost = 0;
  double ms = 0;
};

struct EngineConfig {
  uint64_t budget_bytes = 1536ull << 20;  // candidate storage allowed (see Engine::scan and Engine::narrow)
  uint64_t value_cap = 64'000'000;        // a group keeps values only while it has at most this many positions
  size_t block_size = 16u << 20;          // bytes searched per read; at most 1 GiB
  unsigned workers = 1;                   // threads searching a scan's blocks and comparing a narrow's runs;
                                          // 0: hardware threads - 1, from 1 to 8
};

struct Item {
  uint64_t addr;
  uint64_t bits;          // value bytes, little-endian in the low bits
  bool readable = true;   // false: a hole, a position whose value could not be read (bits 0)
};

struct NarrowItem {
  std::string id;
  Pred pred;
};

class Engine {
 public:
  explicit Engine(ProcessMemory& mem, EngineConfig cfg = {});

  // Searches every writable region for each group. Blocks are read on the calling thread in
  // address order, searched on workers_used() threads, and added to the groups in address order,
  // so the result is the same for any worker count. keep=false replaces all groups; keep=true
  // replaces only groups with a listed id, and the listed groups then follow the others in
  // groups(). Returns the listed groups in request order.
  // After each block of the walk, storage is held within budget_bytes, counting each group's
  // positions and the values it holds: first, listed groups are untracked from the last one
  // backwards until the positions alone fit; then, while positions and values are still over,
  // the values of tracked listed groups are dropped from the last one backwards, each group
  // staying tracked without values. No group is untracked to make room for values. A value
  // buffer also keeps room to grow into, never more than the values it holds, so during the walk
  // the helper's memory can run over budget_bytes by up to the size of the values held. The room
  // is freed when the walk ends; storage_bytes() is then within budget_bytes, unless the groups
  // kept from before are over it alone.
  // Throws EngineError: bad_request (a previous-value predicate or a moved range, align 0, an id
  // listed twice), nothing changed; target_gone (the process is not alive), and the replaced
  // groups are gone.
  std::vector<GroupInfo> scan(const std::vector<GroupSpec>& specs, bool keep, ScanStats& stats);

  // Positions [offset, offset + limit) of a group in address order, with their values read now.
  // Positions that cannot be read are left out, so a page may be short; with holes, each is given
  // in its place as a hole (readable false), so a page holds limit positions unless the group
  // ends. An untracked group gives none. Throws EngineError no_such_group, or target_gone when the
  // process is not alive.
  std::vector<Item> peek(const std::string& id, uint64_t offset, uint64_t limit, bool holes = false);
  // As peek, from the group's first position at or above addr.
  std::vector<Item> peek_from(const std::string& id, uint64_t addr, uint64_t limit, bool holes = false);

  // Keeps, in each listed group, the positions whose value now matches its predicate. A
  // previous-value predicate compares with the values stored at the group's last applied step.
  // Returns applied. With atomic=true, if the listed groups would be left with zero matches in
  // total, nothing changes and it returns false. Groups not listed are untouched. out gets the
  // listed groups in request order, as they stand afterwards.
  // Positions are read in runs shared by the listed groups (see engine.cpp), so groups from one
  // scan read the memory they share once; a position whose memory can no longer be read is
  // dropped and counted in stats.lost, which stays 0 when nothing changes. Runs are read on the
  // calling thread in address order, compared on workers_used() threads, and added to the groups
  // in address order, so the result is the same for any worker count. A worker holds one run's
  // bytes at a time, and only a few more runs than workers are held at once, as a scan holds its
  // blocks. After an applied narrow
  // a group with at most value_cap positions stores the values just read, unless values it did not
  // have before would take storage over budget_bytes.
  // Throws EngineError, with nothing changed: no_such_group (an unknown id, or an untracked
  // group, which has no positions to narrow), bad_request (an id listed twice), no_values (a
  // previous-value predicate or a moved range on a group without values), target_gone (the
  // process is not alive before or after the reads).
  bool narrow(const std::vector<NarrowItem>& items, bool atomic, std::vector<GroupInfo>& out, ScanStats& stats);

  // Removes the listed groups. Throws EngineError no_such_group, removing nothing, if one is unknown.
  void drop(const std::vector<std::string>& ids);

  // Reads one value of the type into the low bytes of bits. Returns false if it cannot be read.
  // Throws EngineError target_gone.
  bool read_value(uint64_t addr, ValueType type, uint64_t& bits);
  // Writes the low bytes of bits as one value of the type, after reading the value there into
  // prev_bits. Returns false if that value cannot be read (nothing is written) or written.
  // After a write, every group with stored values that holds addr as a position of the same
  // width stores the written value there, so its next relation compares with it.
  // Throws EngineError target_gone.
  bool write_value(uint64_t addr, ValueType type, uint64_t bits, uint64_t& prev_bits);

  std::vector<GroupInfo> groups() const;
  uint64_t storage_bytes() const;  // heap bytes held by every group's positions and values
  // Changes the candidate storage budget for later scans and narrows.
  void set_budget_bytes(uint64_t b) { cfg_.budget_bytes = b; }
  uint64_t budget_bytes() const { return cfg_.budget_bytes; }
  unsigned workers_used() const { return workers_; }  // EngineConfig::workers, resolved
  // The most raw 32-bit offsets (matches not yet compacted into positions) that the last scan's
  // workers had room for at once. A worker compacts each 256 KiB slice of a block before the
  // next, so this stays within one slice's worth per worker.
  uint64_t peak_raw_offsets() const { return peak_raw_offsets_; }
  // The most bytes the last scan's listed groups counted toward the budget, their positions and
  // the values they hold, after any block of its walk was added and the budget applied (see
  // scan). With the groups kept from before, that is within budget_bytes unless those alone are
  // over it. The memory really held can be higher, by the value buffers' room to grow, and while
  // a block is being added by that block's matches.
  uint64_t peak_walk_bytes() const { return peak_walk_bytes_; }
  // The most threads that were comparing runs at once during the last narrow; 0 if it threw.
  unsigned narrow_threads_used() const { return narrow_threads_used_; }

 private:
  // Values in position order, each in as many bytes as its type is wide, packed one after
  // another, little-endian (the host's order), so none needs aligning. A moved-from one is empty.
  class Values {
   public:
    explicit Values(size_t width) : width_(width) {}
    Values(Values&& o) noexcept : width_(o.width_) { *this = std::move(o); }
    Values& operator=(Values&& o) noexcept;
    uint64_t get(uint64_t i) const;       // value i in the low bytes, the rest 0
    void set(uint64_t i, uint64_t bits);  // stores bits' low bytes as value i
    void append(const uint8_t* p);        // appends the value whose bytes start at p
    void append(const uint8_t* p, uint64_t n);  // appends n values packed the same way from p
    void clear();                         // removes every value and frees their memory
    void shrink_to_fit();                 // gives back the room beyond the values
    uint64_t bytes() const { return bytes_.capacity(); }  // heap held, room to grow included
    uint64_t used_bytes() const { return used_; }         // what the values themselves take
    const uint8_t* data() const { return bytes_.data(); }  // value i starts i * width bytes in

   private:
    void grow(size_t need);  // doubles the room, or more if that is less than need bytes past the values

    size_t width_;
    size_t used_ = 0;              // the values are bytes_[0, used_); the rest is room to append
    std::vector<uint8_t> bytes_;
  };

  struct Group {
    GroupSpec spec;
    PositionSet positions;
    Values values;  // last-seen values in position order, while has_values
    uint64_t count = 0;
    bool tracked = true;
    bool has_values = true;
    explicit Group(GroupSpec s) : spec(std::move(s)), positions(spec.align), values(width_of(spec.type)) {}
  };

  static GroupInfo info(const Group& g);
  static uint64_t bytes_of(const Group& g);
  static void untrack(Group& g);
  static void forget_values(Group& g);
  size_t index_of(const std::string& id) const;
  const Group& find(const std::string& id) const;
  void add_found(Group& g, PositionSet::Block b, const uint8_t* data);
  void fit_budget(std::vector<Group>& fresh, uint64_t kept);
  std::vector<Group> narrowed(const std::vector<size_t>& at, const std::vector<NarrowItem>& items, ScanStats& stats);
  void fit_gained_values(std::vector<Group>& fresh, const std::vector<size_t>& at);

  ProcessMemory& mem_;
  EngineConfig cfg_;
  unsigned workers_;
  uint64_t peak_raw_offsets_ = 0;
  uint64_t peak_walk_bytes_ = 0;
  unsigned narrow_threads_used_ = 0;
  std::vector<Group> groups_;
};

}  // namespace moth
