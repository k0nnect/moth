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
#include "engine/snapshot.hpp"

namespace moth {

// tracked: the group's positions are stored (otherwise only its count is).
// has_values: a previous-value predicate can be answered: its last-seen values are stored too,
// which happens only while count <= value_cap and the budget has room for them (see Engine::scan
// and Engine::narrow), or it leans on the snapshot (see Engine::snapshot).
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
  // The most writable memory a snapshot copies. While a step over a snapshot runs, the helper can
  // hold up to about this plus twice budget_bytes plus the runs' buffers (see Engine::snapshot).
  uint64_t snapshot_cap_bytes = 8ull << 30;
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

// A group of a snapshot: every place of the type at a multiple of align, which must be a multiple
// of the type's width.
struct SnapSpec {
  std::string id;
  ValueType type;
  uint32_t align;
};

class Engine {
 public:
  explicit Engine(ProcessMemory& mem, EngineConfig cfg = {});

  // Searches every writable region for each group. Blocks are read on the calling thread in
  // address order, searched on workers_used() threads, and added to the groups in address order,
  // so the result is the same for any worker count. keep=false replaces all groups; keep=true
  // replaces only groups with a listed id, and the listed groups then follow the others in
  // groups(). Returns the listed groups in request order. The groups replaced are released before
  // anything is read, and so is the snapshot once no group leans on it: always with keep=false.
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

  // Copies every writable region into a new snapshot, and makes each listed group every place of
  // its type, at a multiple of its align, whose bytes were copied: tracked, with has_values true,
  // leaning on the snapshot for its previous values. Returns them in request order; they replace
  // every group. First the regions' sizes are added up: over snapshot_cap_bytes is refused before
  // anything is released or read. Then every group and any earlier snapshot are released, so two
  // snapshots are never held together, and each region is read in blocks on the calling thread,
  // as a scan reads, into a buffer of its own; only the bytes that arrived are held (bytes not read
  // are counted in stats.unreadable_bytes, and are no place of any group). A place's bytes lie in
  // one block of the walk; blocks are block_size rounded down to a multiple of 8, so in a region
  // that starts and ends at a multiple of 8, as a process's regions do, every aligned place whose
  // bytes were read is one. stats is filled as by a scan. Groups whose positions do not fit
  // budget_bytes are untracked from the last one backwards, after each block as in a scan; the
  // snapshot is not counted in storage_bytes() or the budget. If no listed group is left tracked,
  // the snapshot is released.
  // Memory: between steps the helper holds the snapshot (at most snapshot_cap_bytes) and candidate
  // storage within budget_bytes. While a step runs (see narrow) it also holds the step's new
  // positions and any values gained, within budget_bytes again, and the runs' buffers,
  // (workers_used() + 4) * block_size. So the peak is about the snapshot plus twice budget_bytes
  // plus those buffers. The new positions are built beside the old ones because an atomic step
  // that keeps nothing, and a step that fails part way, must leave the old positions intact.
  // Throws EngineError, with nothing changed: bad_request (align 0 or not a multiple of the type's
  // width, an id listed twice), too_big "<bytes> to snapshot, limit <bytes>" (both plain decimal
  // byte counts), target_gone (the process is not alive). With every group and the snapshot
  // released: too_big "could not get memory for the snapshot", target_gone (the process closed
  // during the walk), or what a read threw.
  std::vector<GroupInfo> snapshot(const std::vector<SnapSpec>& specs, ScanStats& stats);

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
  // blocks; beyond those bytes the runs hold a bounded amount (see peak_narrow_held_bytes), and no
  // group has more values copied than value_cap allows. After an applied narrow
  // a group with at most value_cap positions stores the values just read, unless values it did not
  // have before would take storage over budget_bytes.
  // A group that leans on the snapshot takes each place's previous value from the snapshot's bytes
  // there. The worker comparing a run reads them and, once every listed group in the run is
  // compared, replaces in place the snapshot's bytes at each place such a group kept with the bytes
  // just read, so its next relation compares with this step; it copies none of their values. Places
  // no group keeps are left as they were, and are no longer candidates. A step that is not applied
  // kept nothing, so it replaced nothing. After an applied step, such a group with at most
  // value_cap positions takes its values from the snapshot, which are the bytes just read, and no
  // longer leans on it; if those values would take storage over budget_bytes, it takes none and
  // goes on leaning on the snapshot, with has_values still true. The snapshot is released once no
  // group leans on it. A group leaning on it that a narrow does not list still compares with the
  // snapshot, which by then may hold newer bytes at places other groups kept.
  // Memory: until a step is known to be applied without error, the old positions stay as they
  // were, so the new positions are built beside them (within budget_bytes, since a group's
  // positions only shrink) and values gained are taken while both are held. With a snapshot the
  // peak is therefore about the snapshot plus twice budget_bytes plus the runs' buffers,
  // (workers_used() + 4) * block_size; between steps it is the snapshot plus budget_bytes.
  // Throws EngineError, with nothing changed: no_such_group (an unknown id, or an untracked
  // group, which has no positions to narrow), bad_request (an id listed twice), no_values (a
  // previous-value predicate or a moved range on a group without values), target_gone (the
  // process is not alive before or after the reads). If a narrow that has handed out a run
  // leaning on the snapshot then throws (target_gone after the reads, or what a read or a worker
  // threw), the snapshot is released instead and no group leans on it any more: their positions
  // are unchanged and they have no values, so they never compare with bytes from two moments.
  bool narrow(const std::vector<NarrowItem>& items, bool atomic, std::vector<GroupInfo>& out, ScanStats& stats);

  // Removes the listed groups, and the snapshot once no group leans on it. Throws EngineError
  // no_such_group, removing nothing, if one is unknown.
  void drop(const std::vector<std::string>& ids);

  // Reads one value of the type into the low bytes of bits. Returns false if it cannot be read.
  // Throws EngineError target_gone.
  bool read_value(uint64_t addr, ValueType type, uint64_t& bits);
  // Writes the low bytes of bits as one value of the type, after reading the value there into
  // prev_bits. Returns false if that value cannot be read (nothing is written) or written.
  // After a write, every group with stored values that holds addr as a position of the same
  // width stores the written value there, so its next relation compares with it, and the
  // snapshot holds the written bytes wherever it holds those addresses.
  // Throws EngineError target_gone.
  bool write_value(uint64_t addr, ValueType type, uint64_t bits, uint64_t& prev_bits);

  std::vector<GroupInfo> groups() const;
  uint64_t storage_bytes() const;  // heap bytes held by every group's positions and values, not the snapshot
  uint64_t snapshot_bytes() const { return snapshot_.bytes(); }  // heap bytes the snapshot holds; 0 when none
  const Snapshot& held_snapshot() const { return snapshot_; }  // its bytes as they stand, for checks
  // Changes the snapshot limit for later snapshots.
  void set_snapshot_cap_bytes(uint64_t b) { cfg_.snapshot_cap_bytes = b; }
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
  // The most bytes the last narrow's runs held at once beyond their own bytes, whether out with a
  // worker or back for reuse: room for the positions kept in blocks split across runs, and for the
  // values copied. It stays within (workers_used() + 4) * block_size unless one run's kept
  // positions alone need more. The narrowed blocks the workers build, the groups' new positions,
  // are not counted.
  uint64_t peak_narrow_held_bytes() const { return peak_narrow_held_bytes_; }

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
    uint8_t* extend(uint64_t n);          // appends n values, 0 until written at the pointer returned
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
    bool snap = false;  // no values of its own: its previous values are the snapshot's bytes at its positions
    explicit Group(GroupSpec s) : spec(std::move(s)), positions(spec.align), values(width_of(spec.type)) {}
  };

  static GroupInfo info(const Group& g);
  static uint64_t bytes_of(const Group& g);
  static void untrack(Group& g);
  static void forget_values(Group& g);
  std::vector<Group> copy_regions(const std::vector<Region>& regions, const std::vector<SnapSpec>& specs,
                                  ScanStats& stats);
  bool take_values_from_snapshot(Group& g);
  void forget_snapshot();
  void release_unused_snapshot();
  size_t index_of(const std::string& id) const;
  const Group& find(const std::string& id) const;
  void add_found(Group& g, PositionSet::Block b, const uint8_t* data);
  void fit_budget(std::vector<Group>& fresh, uint64_t kept);
  std::vector<Group> narrowed(const std::vector<size_t>& at, const std::vector<NarrowItem>& items, ScanStats& stats,
                              bool& replacing);
  void fit_gained_values(std::vector<Group>& fresh, const std::vector<size_t>& at);

  ProcessMemory& mem_;
  EngineConfig cfg_;
  unsigned workers_;
  uint64_t peak_raw_offsets_ = 0;
  uint64_t peak_walk_bytes_ = 0;
  unsigned narrow_threads_used_ = 0;
  uint64_t peak_narrow_held_bytes_ = 0;
  std::vector<Group> groups_;
  Snapshot snapshot_;
};

}  // namespace moth
