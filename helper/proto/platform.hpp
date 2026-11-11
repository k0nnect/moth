#pragma once
// What the protocol handler needs from the operating system. Portable C++20, no OS calls;
// the Windows helper implements it, the tests fake it.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/memory.hpp"
#include "engine/types.hpp"

namespace moth {

struct ProcInfo {
  uint32_t pid;
  std::string name;
  int bits;
};

class Platform {
 public:
  virtual ~Platform() = default;
  virtual std::vector<ProcInfo> processes() = 0;
  virtual std::vector<std::string> modules(uint32_t pid) = 0;
  // Throws EngineError{"no_such_process"} or {"access_denied"}.
  virtual std::unique_ptr<ProcessMemory> open(uint32_t pid, ProcInfo& out) = 0;
};

}  // namespace moth
