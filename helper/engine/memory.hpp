#pragma once
// The target process's memory as the engine sees it. Portable C++20, no OS calls; the
// Windows implementation and the test fake live outside engine/.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace moth {

struct Region {
  uint64_t base;
  uint64_t size;
};

class ProcessMemory {
 public:
  virtual ~ProcessMemory() = default;
  virtual std::vector<Region> writable_regions() = 0;
  virtual size_t read(uint64_t addr, void* out, size_t len) = 0;  // bytes read, 0 on failure
  virtual bool write(uint64_t addr, const void* data, size_t len) = 0;
  virtual bool alive() = 0;
};

}  // namespace moth
