// Stand-in "game" for automated tests: holds known values in memory and lets the test harness
// read or change them over a TCP control port, so scans can be checked against ground truth.
//   moth-target.exe <port> [--filler-mb N] [--filler-small-mb N] [--filler-tiny-mb N] [--filler-half-mb N]
// Fillers, N MiB each: random bytes; int32 values 0 to 7 at random (small); int32 values cycling
// 1 to 4096 (tiny), which read as floats are all too small to be anything; floats of 0.5 (half).
// Control lines: "get <name>", "set <name> <value>", "free <page>" (decommits that page of the
// small filler, or of the large one when there is no small one, as a game freeing memory would),
// "quit". Replies: the value, "ok" or "error".
// A thread works total out from base + bonus every 20 ms, as a game recalculates a number.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "nlohmann/json.hpp"

namespace {

#pragma pack(push, 1)
struct Packed {
  uint8_t pad;   // pushes gems onto an odd address
  int32_t gems;
};
#pragma pack(pop)

// Everything lives in one heap block, padded so nothing else sits next to it.
struct Vars {
  float health;
  int32_t health_copy;
  uint8_t ammo;
  int16_t level16;
  int64_t xp64;
  double morale;
  int32_t money_cents;
  int32_t lives;
  Packed packed;
  char gap[4096];  // keeps the values above out of reach of a near listing of the sheet
  // a character sheet: five int32 side by side, the fourth unnamed
  int32_t xp;
  int32_t level;
  int32_t points;
  int32_t unnamed;
  int32_t cents;
  int32_t base;
  int32_t bonus;
  int32_t total;  // base + bonus, rewritten by the recalc thread
  float charge;
  float drift;
  float stamina;  // a value whose number is never typed: found by how it moves
  char tail[4096];
};

enum class Kind { F32, I32, U8, I16, I64, F64 };

struct Var {
  const char* name;
  Kind kind;
  void* ptr;
};

std::string format(const Var& v) {
  char b[64];
  switch (v.kind) {
    case Kind::F32: std::snprintf(b, sizeof b, "%.9g", static_cast<double>(*static_cast<volatile float*>(v.ptr))); break;
    case Kind::F64: std::snprintf(b, sizeof b, "%.17g", *static_cast<volatile double*>(v.ptr)); break;
    case Kind::I32: std::snprintf(b, sizeof b, "%d", *static_cast<volatile int32_t*>(v.ptr)); break;
    case Kind::U8: std::snprintf(b, sizeof b, "%u", static_cast<unsigned>(*static_cast<volatile uint8_t*>(v.ptr))); break;
    case Kind::I16: std::snprintf(b, sizeof b, "%d", static_cast<int>(*static_cast<volatile int16_t*>(v.ptr))); break;
    case Kind::I64: std::snprintf(b, sizeof b, "%lld", static_cast<long long>(*static_cast<volatile int64_t*>(v.ptr))); break;
  }
  return b;
}

bool assign(const Var& v, const std::string& text) {
  char* end = nullptr;
  if (text.empty()) return false;
  switch (v.kind) {
    case Kind::F32: { float x = std::strtof(text.c_str(), &end); *static_cast<volatile float*>(v.ptr) = x; break; }
    case Kind::F64: { double x = std::strtod(text.c_str(), &end); *static_cast<volatile double*>(v.ptr) = x; break; }
    case Kind::I32: { long long x = std::strtoll(text.c_str(), &end, 10); *static_cast<volatile int32_t*>(v.ptr) = static_cast<int32_t>(x); break; }
    case Kind::U8: { long long x = std::strtoll(text.c_str(), &end, 10); *static_cast<volatile uint8_t*>(v.ptr) = static_cast<uint8_t>(x); break; }
    case Kind::I16: { long long x = std::strtoll(text.c_str(), &end, 10); *static_cast<volatile int16_t*>(v.ptr) = static_cast<int16_t>(x); break; }
    case Kind::I64: { long long x = std::strtoll(text.c_str(), &end, 10); *static_cast<volatile int64_t*>(v.ptr) = static_cast<int64_t>(x); break; }
  }
  return end && *end == '\0';
}

const char* type_name(Kind k) {
  switch (k) {
    case Kind::F32: return "f32";
    case Kind::F64: return "f64";
    case Kind::I32: return "i32";
    case Kind::U8: return "u8";
    case Kind::I16: return "i16";
    case Kind::I64: return "i64";
  }
  return "";
}

struct XorShift {
  uint64_t s = 0x6d6f7468;
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
};

enum Filler { kRandom, kSmall, kTiny, kHalf, kFillers };

// One committed read-write block of mb MiB; the pointer is kept so the block stays live.
void* make_filler(size_t mb, Filler kind) {
  size_t bytes = mb << 20;
  void* p = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!p) return nullptr;
  XorShift g;
  if (kind == kSmall) {
    auto* w = static_cast<uint32_t*>(p);
    for (size_t i = 0; i < bytes / 4; i++) w[i] = static_cast<uint32_t>(g.next() % 8);
  } else if (kind == kTiny) {
    auto* w = static_cast<uint32_t*>(p);
    for (size_t i = 0; i < bytes / 4; i++) w[i] = static_cast<uint32_t>(i % 4096 + 1);
  } else if (kind == kHalf) {
    auto* f = static_cast<float*>(p);
    for (size_t i = 0; i < bytes / 4; i++) f[i] = 0.5f;
  } else {
    auto* w = static_cast<uint64_t*>(p);
    for (size_t i = 0; i < bytes / 8; i++) w[i] = g.next();
  }
  return p;
}

void* volatile g_fillers[kFillers] = {};  // keeps the filler blocks referenced
size_t g_filler_bytes[kFillers] = {};

// Rewrites total from base and bonus every 20 ms, for as long as the process runs.
DWORD WINAPI recalc(void* p) {
  auto* v = static_cast<Vars*>(p);
  auto* base = static_cast<volatile int32_t*>(&v->base);
  auto* bonus = static_cast<volatile int32_t*>(&v->bonus);
  auto* total = static_cast<volatile int32_t*>(&v->total);
  for (;;) {
    *total = *base + *bonus;
    Sleep(20);
  }
}

// Decommits page number `text` of the small filler (the large one when there is no small one).
bool free_page(const std::string& text) {
  const int which = g_fillers[kSmall] ? kSmall : kRandom;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  const size_t page = si.dwPageSize;
  char* end = nullptr;
  const unsigned long long n = std::strtoull(text.c_str(), &end, 10);
  if (!g_fillers[which] || text.empty() || *end != '\0' || n >= g_filler_bytes[which] / page) return false;
  return VirtualFree(static_cast<char*>(g_fillers[which]) + n * page, page, MEM_DECOMMIT) != 0;
}

bool send_all(SOCKET s, const std::string& data) {
  size_t off = 0;
  while (off < data.size()) {
    int n = send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: moth-target <port> [--filler-mb N] [--filler-small-mb N] [--filler-tiny-mb N] [--filler-half-mb N]\n");
    return 2;
  }
  int port = std::atoi(argv[1]);
  for (int i = 2; i + 1 < argc; i += 2) {
    Filler kind;
    if (std::strcmp(argv[i], "--filler-mb") == 0)
      kind = kRandom;
    else if (std::strcmp(argv[i], "--filler-small-mb") == 0)
      kind = kSmall;
    else if (std::strcmp(argv[i], "--filler-tiny-mb") == 0)
      kind = kTiny;
    else if (std::strcmp(argv[i], "--filler-half-mb") == 0)
      kind = kHalf;
    else
      continue;
    size_t mb = static_cast<size_t>(std::atoll(argv[i + 1]));
    if (mb == 0) continue;
    void* p = make_filler(mb, kind);
    if (!p) {
      std::fprintf(stderr, "moth-target: cannot allocate filler\n");
      return 2;
    }
    g_fillers[kind] = p;
    g_filler_bytes[kind] = mb << 20;
  }

  Vars* vars = new Vars();
  std::memset(vars, 0, sizeof *vars);
  vars->health = 100.0f;
  vars->health_copy = 100;
  vars->ammo = 30;
  vars->level16 = 7;
  vars->xp64 = 100000;
  vars->morale = 85.3;
  vars->money_cents = 13250;
  vars->packed.gems = 4242;
  vars->lives = 3;
  vars->xp = 15;
  vars->level = 2;
  vars->points = 1;
  vars->unnamed = 0;
  vars->cents = 532;
  vars->base = 5;
  vars->bonus = 1;
  vars->total = vars->base + vars->bonus;
  vars->charge = 5.0f;
  vars->drift = 5.5f;
  vars->stamina = 62.5f;

  Var table[] = {
      {"health", Kind::F32, &vars->health},
      {"health_copy", Kind::I32, &vars->health_copy},
      {"ammo", Kind::U8, &vars->ammo},
      {"level16", Kind::I16, &vars->level16},
      {"xp64", Kind::I64, &vars->xp64},
      {"morale", Kind::F64, &vars->morale},
      {"money_cents", Kind::I32, &vars->money_cents},
      {"gems", Kind::I32, &vars->packed.gems},
      {"lives", Kind::I32, &vars->lives},
      {"xp", Kind::I32, &vars->xp},
      {"level", Kind::I32, &vars->level},
      {"points", Kind::I32, &vars->points},
      {"cents", Kind::I32, &vars->cents},
      {"base", Kind::I32, &vars->base},
      {"bonus", Kind::I32, &vars->bonus},
      {"total", Kind::I32, &vars->total},
      {"charge", Kind::F32, &vars->charge},
      {"drift", Kind::F32, &vars->drift},
      {"stamina", Kind::F32, &vars->stamina},
  };

  HANDLE recalc_thread = CreateThread(nullptr, 0, recalc, vars, 0, nullptr);
  if (!recalc_thread) {
    std::fprintf(stderr, "moth-target: cannot start the recalc thread\n");
    return 2;
  }
  CloseHandle(recalc_thread);  // it runs until the process ends

  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 2;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<u_short>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (s == INVALID_SOCKET || connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    std::fprintf(stderr, "moth-target: cannot connect to 127.0.0.1:%d\n", port);
    return 2;
  }
  BOOL on = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);

  nlohmann::json vars_json = nlohmann::json::array();
  for (auto& v : table) {
    char a[32];
    std::snprintf(a, sizeof a, "0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(v.ptr)));
    std::string text = format(v);
    nlohmann::json value;
    if (v.kind == Kind::F32 || v.kind == Kind::F64)
      value = std::strtod(text.c_str(), nullptr);
    else
      value = text;
    vars_json.push_back({{"name", v.name}, {"type", type_name(v.kind)}, {"addr", a}, {"value", value}});
  }
  if (!send_all(s, nlohmann::json{{"vars", vars_json}}.dump() + "\n")) return 2;

  std::string buf;
  char chunk[4096];
  bool quit = false;
  while (!quit) {
    int n = recv(s, chunk, sizeof chunk, 0);
    if (n <= 0) break;
    buf.append(chunk, static_cast<size_t>(n));
    size_t nl;
    while (!quit && (nl = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::string reply;
      if (line == "quit") {
        quit = true;
        break;
      }
      size_t sp = line.find(' ');
      std::string cmd = line.substr(0, sp);
      std::string rest = sp == std::string::npos ? "" : line.substr(sp + 1);
      std::string name = rest.substr(0, rest.find(' '));
      std::string val = rest.find(' ') == std::string::npos ? "" : rest.substr(rest.find(' ') + 1);
      const Var* found = nullptr;
      for (auto& v : table)
        if (name == v.name) found = &v;
      if (cmd == "free")
        reply = free_page(name) ? "ok" : "error";
      else if (!found || (cmd != "set" && cmd != "get"))
        reply = "error";
      else if (cmd == "get")
        reply = format(*found);
      else
        reply = assign(*found, val) ? "ok" : "error";
      if (!send_all(s, reply + "\n")) quit = true;
    }
  }
  closesocket(s);
  WSACleanup();
  return 0;
}
