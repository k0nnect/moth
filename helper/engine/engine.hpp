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
// has_values: its last-seen values are stored too, which happens only while count <= value_cap.
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
// only bytes that really cannot be read are left out (see read_spans in engine.cpp).
struct ScanStats {
  uint64_t regions = 0, bytes = 0, unreadable_bytes = 0, lost = 0;
  double ms = 0;
};

struct EngineConfig {
  uint64_t budget_bytes = 1536ull << 20;  // candidate storage allowed once a scan ends
  uint64_t value_cap = 2'000'000;         // a group keeps values only while it has at most this many positions
  size_t block_size = 16u << 20;          // bytes searched per read; at most 1 GiB
  unsigned workers = 1;                   // threads searching a scan's blocks; 0: hardware threads - 1, from 1 to 8
};

struct Item {
  uint64_t addr;
  uint64_t bits;  // value bytes, little-endian in the low bits
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
  // groups(). Returns the listed groups in request order. If the storage is then over
  // budget_bytes, listed groups are untracked from the last one backwards until it fits.
  // Throws EngineError: bad_request (a previous-value predicate, align 0, an id listed twice),
  // nothing changed; target_gone (the process is not alive), and the replaced groups are gone.
  std::vector<GroupInfo> scan(const std::vector<GroupSpec>& specs, bool keep, ScanStats& stats);

  // Positions [offset, offset + limit) of a group in address order, with their values read now.
  // Positions that cannot be read are left out, so a page may be short. An untracked group
  // gives none. Throws EngineError no_such_group.
  std::vector<Item> peek(const std::string& id, uint64_t offset, uint64_t limit);

  // Keeps, in each listed group, the positions whose value now matches its predicate. A
  // previous-value predicate compares with the values stored at the group's last applied step.
  // Returns applied. With atomic=true, if the listed groups would be left with zero matches in
  // total, nothing changes and it returns false. Groups not listed are untouched. out gets the
  // listed groups in request order, as they stand afterwards.
  // Positions are read in runs shared by the listed groups (see engine.cpp), so groups from one
  // scan read the memory they share once; a position whose memory can no longer be read is
  // dropped and counted in stats.lost, which stays 0 when nothing changes. After an applied narrow
  // a group with at most value_cap positions stores the values just read, unless values it did not
  // have before would take storage over budget_bytes.
  // Throws EngineError, with nothing changed: no_such_group (an unknown id, or an untracked
  // group, which has no positions to narrow), bad_request (an id listed twice), no_values (a
  // previous-value predicate on a group without values), target_gone (the process is not alive
  // before or after the reads).
  bool narrow(const std::vector<NarrowItem>& items, bool atomic, std::vector<GroupInfo>& out, ScanStats& stats);

  // Removes the listed groups. Throws EngineError no_such_group, removing nothing, if one is unknown.
  void drop(const std::vector<std::string>& ids);

  // Reads one value of the type into the low bytes of bits. Returns false if it cannot be read.
  // Throws EngineError target_gone.
  bool read_value(uint64_t addr, ValueType type, uint64_t& bits);
  // Writes the low bytes of bits as one value of the type, after reading the value there into
  // prev_bits. Returns false if that value cannot be read (nothing is written) or written.
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

 private:
  struct Group {
    GroupSpec spec;
    PositionSet positions;
    std::vector<uint64_t> values;  // last-seen values in position order, while has_values
    uint64_t count = 0;
    bool tracked = true;
    bool has_values = true;
    explicit Group(GroupSpec s) : spec(std::move(s)), positions(spec.align) {}
  };

  static GroupInfo info(const Group& g);
  static uint64_t bytes_of(const Group& g);
  static void untrack(Group& g);
  size_t index_of(const std::string& id) const;
  const Group& find(const std::string& id) const;
  void add_found(Group& g, PositionSet::Block b, const uint8_t* data);
  void fit_budget(std::vector<Group>& fresh, uint64_t kept, bool with_values);
  std::vector<Group> narrowed(const std::vector<size_t>& at, const std::vector<NarrowItem>& items, ScanStats& stats);
  void fit_gained_values(std::vector<Group>& fresh, const std::vector<size_t>& at);

  ProcessMemory& mem_;
  EngineConfig cfg_;
  unsigned workers_;
  uint64_t peak_raw_offsets_ = 0;
  std::vector<Group> groups_;
};

}  // namespace moth
