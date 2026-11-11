// Platform over Toolhelp snapshots and OpenProcess.
#include <windows.h>
#include <tlhelp32.h>

#include "engine/types.hpp"
#include "win/win_internal.hpp"
#include "win/win_memory.hpp"

namespace moth {

namespace {

std::string utf8(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  s.resize(static_cast<size_t>(n) - 1);
  return s;
}

int bits_of(HANDLE h) {
  BOOL wow = FALSE;
  return IsWow64Process(h, &wow) && wow ? 32 : 64;
}

class WinPlatform : public Platform {
 public:
  std::vector<ProcInfo> processes() override {
    std::vector<ProcInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
      out.push_back({pe.th32ProcessID, utf8(pe.szExeFile), 0});
    CloseHandle(snap);
    return out;
  }

  // Every module loaded in pid. A list that cannot be read is an error, never an empty list, so
  // the anti-cheat check fails closed. The snapshot fails with ERROR_BAD_LENGTH while the process
  // is loading or unloading a module; that is tried again a few times.
  std::vector<std::string> modules(uint32_t pid) override {
    auto failed = [&](DWORD e) {
      return EngineError("modules_failed", "cannot list the modules of pid " + std::to_string(pid) + ", error " + std::to_string(e));
    };
    HANDLE snap;
    for (int tries = 1;; tries++) {
      snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
      if (snap != INVALID_HANDLE_VALUE) break;
      const DWORD e = GetLastError();
      if (e != ERROR_BAD_LENGTH || tries == 5) throw failed(e);
      Sleep(10);
    }
    std::vector<std::string> out;
    MODULEENTRY32W me;
    me.dwSize = sizeof me;
    BOOL ok = Module32FirstW(snap, &me);
    const DWORD first_error = ok ? 0 : GetLastError();  // every process has its own module at least
    for (; ok; ok = Module32NextW(snap, &me)) out.push_back(utf8(me.szModule));
    CloseHandle(snap);
    if (out.empty()) throw failed(first_error);
    return out;
  }

  std::unique_ptr<ProcessMemory> open(uint32_t pid, ProcInfo& out) override {
    std::string name;
    bool found = false;
    for (auto& p : processes())
      if (p.pid == pid) {
        name = p.name;
        found = true;
        break;
      }
    if (!found) throw EngineError("no_such_process", "no process with pid " + std::to_string(pid));
    HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!h)
      h = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION |
                          SYNCHRONIZE,
                      FALSE, pid);
    if (!h) {
      DWORD e = GetLastError();
      if (e == ERROR_INVALID_PARAMETER) throw EngineError("no_such_process", "no process with pid " + std::to_string(pid));
      throw EngineError("access_denied", "cannot open pid " + std::to_string(pid) + ", error " + std::to_string(e));
    }
    out = {pid, name, bits_of(h)};
    return std::make_unique<WinMemory>(h);
  }
};

}  // namespace

std::unique_ptr<Platform> make_win_platform() { return std::make_unique<WinPlatform>(); }

}  // namespace moth
