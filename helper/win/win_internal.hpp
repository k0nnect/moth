#pragma once
#include <windows.h>

#include "engine/memory.hpp"

namespace moth {

class WinMemory : public ProcessMemory {
 public:
  explicit WinMemory(HANDLE process);  // takes ownership of the handle
  ~WinMemory() override;
  WinMemory(const WinMemory&) = delete;
  WinMemory& operator=(const WinMemory&) = delete;
  std::vector<Region> writable_regions() override;
  std::vector<Region> readable_regions() override;
  size_t read(uint64_t addr, void* out, size_t len) override;
  bool write(uint64_t addr, const void* data, size_t len) override;
  bool alive() override;

 private:
  HANDLE process_;
};

}  // namespace moth
