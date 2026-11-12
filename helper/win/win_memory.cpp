// ProcessMemory over a Win32 process handle: reads and writes from outside, nothing is injected.
#include <windows.h>

#include "win/win_internal.hpp"

namespace moth {

namespace {
bool writable_protection(DWORD p) {
  if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
  DWORD base = p & 0xff;
  return base == PAGE_READWRITE || base == PAGE_WRITECOPY || base == PAGE_EXECUTE_READWRITE ||
         base == PAGE_EXECUTE_WRITECOPY;
}
bool readable_protection(DWORD p) { return (p & (PAGE_GUARD | PAGE_NOACCESS)) == 0; }

// The committed regions whose protection passes keep, ascending, with touching ones joined.
std::vector<Region> committed_regions(HANDLE process, bool (*keep)(DWORD)) {
  std::vector<Region> out;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  uint64_t addr = reinterpret_cast<uint64_t>(si.lpMinimumApplicationAddress);
  const uint64_t limit = reinterpret_cast<uint64_t>(si.lpMaximumApplicationAddress);
  while (addr < limit) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi) != sizeof mbi) break;
    uint64_t base = reinterpret_cast<uint64_t>(mbi.BaseAddress);
    uint64_t size = mbi.RegionSize;
    if (size == 0) break;
    if (mbi.State == MEM_COMMIT && keep(mbi.Protect)) {
      if (!out.empty() && out.back().base + out.back().size == base)
        out.back().size += size;
      else
        out.push_back({base, size});
    }
    uint64_t next = base + size;
    if (next <= addr) break;
    addr = next;
  }
  return out;
}
}  // namespace

WinMemory::WinMemory(HANDLE process) : process_(process) {}

WinMemory::~WinMemory() {
  if (process_) CloseHandle(process_);
}

std::vector<Region> WinMemory::writable_regions() { return committed_regions(process_, writable_protection); }

// No-access and guard pages are committed but fail a read.
std::vector<Region> WinMemory::readable_regions() { return committed_regions(process_, readable_protection); }

size_t WinMemory::read(uint64_t addr, void* out, size_t len) {
  SIZE_T got = 0;
  if (!ReadProcessMemory(process_, reinterpret_cast<LPCVOID>(addr), out, len, &got)) return 0;
  return got;
}

bool WinMemory::write(uint64_t addr, const void* data, size_t len) {
  SIZE_T put = 0;
  return WriteProcessMemory(process_, reinterpret_cast<LPVOID>(addr), data, len, &put) && put == len;
}

bool WinMemory::alive() { return WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }

}  // namespace moth
