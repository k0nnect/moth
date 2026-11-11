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
// ms: wall-clock time of the step.
struct ScanStats {
  uint64_t regions = 0, bytes = 0, unreadable_bytes = 0, lost = 0;
  double ms = 0;
};

struct EngineConfig {
  uint64_t budget_bytes = 1536ull << 20;  // candidate storage allowed once a scan ends
  uint64_t value_cap = 2'000'000;         // a group keeps values only while it has at most this many positions
  size_t block_size = 16u << 20;          // bytes searched per read; at most 1 GiB
  unsigned workers = 1;
};

struct Item {
  uint64_t addr;
  uint64_t bits;  // value bytes, little-endian in the low bits
};

class Engine {
 public:
  explicit Engine(ProcessMemory& mem, EngineConfig cfg = {});

  // Searches every writable region for each group. keep=false replaces all groups; keep=true
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

  std::vector<GroupInfo> groups() const;
  uint64_t storage_bytes() const;  // heap bytes held by every group's positions and values

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
  const Group& find(const std::string& id) const;
  void add_hits(Group& g, uint64_t base, size_t span, const uint8_t* data, const std::vector<uint32_t>& offsets);
  void fit_budget(std::vector<Group>& fresh, uint64_t kept, bool with_values);

  ProcessMemory& mem_;
  EngineConfig cfg_;
  std::vector<Group> groups_;
};

}  // namespace moth
