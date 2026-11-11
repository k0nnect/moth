#pragma once
// Test-only Platform: one FakeMemory per added process.
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "proto/platform.hpp"
#include "test/fake_memory.hpp"

namespace moth::test {

class FakePlatform : public Platform {
 public:
  void add(uint32_t pid, const std::string& name, int bits) {
    procs_[pid] = Proc{ProcInfo{pid, name, bits}, std::make_unique<FakeMemory>(), {}};
  }
  FakeMemory& mem(uint32_t pid) { return *procs_.at(pid).mem; }
  void set_modules(uint32_t pid, std::vector<std::string> m) { procs_.at(pid).modules = std::move(m); }
  void deny(uint32_t pid) { denied_.insert(pid); }
  void fail_modules(uint32_t pid) { modules_fail_.insert(pid); }  // as when the snapshot cannot be taken

  std::vector<ProcInfo> processes() override {
    std::vector<ProcInfo> out;
    for (auto& [pid, p] : procs_) out.push_back(p.info);
    return out;
  }
  std::vector<std::string> modules(uint32_t pid) override {
    auto it = procs_.find(pid);
    if (it == procs_.end()) throw EngineError("no_such_process", "no process " + std::to_string(pid));
    if (modules_fail_.count(pid)) throw EngineError("modules_failed", "cannot list the modules of " + std::to_string(pid));
    return it->second.modules;
  }
  std::unique_ptr<ProcessMemory> open(uint32_t pid, ProcInfo& out) override {
    auto it = procs_.find(pid);
    if (it == procs_.end()) throw EngineError("no_such_process", "no process " + std::to_string(pid));
    if (denied_.count(pid)) throw EngineError("access_denied", "cannot open " + std::to_string(pid));
    out = it->second.info;
    return std::make_unique<Forward>(*it->second.mem);
  }

 private:
  // The handler owns what open() returns while the test keeps poking the same memory.
  struct Forward : ProcessMemory {
    FakeMemory& m;
    explicit Forward(FakeMemory& f) : m(f) {}
    std::vector<Region> writable_regions() override { return m.writable_regions(); }
    size_t read(uint64_t a, void* o, size_t n) override { return m.read(a, o, n); }
    bool write(uint64_t a, const void* d, size_t n) override { return m.write(a, d, n); }
    bool alive() override { return m.alive(); }
  };
  struct Proc {
    ProcInfo info;
    std::unique_ptr<FakeMemory> mem;
    std::vector<std::string> modules;
  };
  std::map<uint32_t, Proc> procs_;
  std::set<uint32_t> denied_, modules_fail_;
};

}  // namespace moth::test
