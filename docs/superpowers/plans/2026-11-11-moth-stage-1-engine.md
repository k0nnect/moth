# moth stage 1 (the engine) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** From the terminal, attach to a game running in a CrossOver bottle, find a value by typing only the number on screen, and change it, proven on a stand-in target and then on Disco Elysium.

**Architecture:** Three processes. A C++ helper (`moth-helper.exe`, Windows x86_64) runs inside the game's bottle, holds every candidate and does all memory reads, comparisons and writes. An OCaml core runs natively as a small background daemon: it turns the typed number into a list of interpretations, decides how to narrow, widen and fall back, and owns the hunt. The `moth` command is a thin client of the daemon. Helper and core talk newline-delimited JSON over a loopback TCP connection the core listens on; client and core talk the same way over a Unix socket.

**Tech Stack:** OCaml 5.5.x, dune, yojson, alcotest (tests only). C++20, CMake, mingw-w64 (GCC 16.2) for the Windows build, Apple clang for native unit tests, doctest and nlohmann/json vendored as single headers.

**Spec:** `docs/superpowers/specs/2026-11-11-moth-design.md`. Read sections 5, 6.4, 7 and 10 before starting any task.

## Global Constraints

- Languages for moth's own source: OCaml and C++ only. Build files (Makefile, CMake, dune) are fine. No Python, no shell scripts as source files.
- System Integrity Protection stays on. Nothing may need `sudo`. CrossOver is never re-signed or modified.
- moth only reads and writes the target's memory from outside. No code is loaded into the target, no memory allocated in it, no hooks.
- The user never supplies a data type, on any command.
- Launch form for anything run in a bottle: `"<CrossOver>/bin/wine" --bottle "<bottle>" --workdir "<dir>" "<absolute macOS path to .exe>" <args>`. A relative `.exe` path fails.
- CrossOver writes its own lines to a launched program's stdout and stderr. Never parse a bottle program's stdout.
- No GPL dependencies. Permissive licences only.
- User-facing text is lowercase, plain, no emojis.
- Private: local git only. Never add a remote, push or publish.
- Commits: lowercase messages, no attribution trailers (Chris's recorded preference for code under `k0nnect`).
- Do not edit the user's global configuration. `opam init` must be run with `--no-setup`.
- The brainstorm's probe programs are not to be reused. Write everything fresh.
- All paths below are relative to `~/Documents/moth`.

## Review Focus

1. **Tiny, very common numbers (0, 1, 3).** A hunt for "3" must still narrow down and must not exhaust memory. Disco Elysium's health is a number like this. Pinned in Task 3 (budget and dense storage) and Task 12 (scenario 9).
2. **The game closes or frees memory in the middle of a hunt.** The user gets "the game closed" or a count of lost matches, never a hang or a crash. Pinned in Task 4 and Task 9.
3. **Numbers typed the way they look on screen.** "1,250", " 85 ", "+5", "-3", "132.50" work; "85%", "abc", "" are refused with a plain message. Pinned in Task 8.
4. **Things missing or stale at startup.** CrossOver not found, a bottle name that doesn't exist, a helper that never connects, a stale socket file from a dead daemon. Each gives a plain error within its timeout. Pinned in Tasks 10 and 11.
5. **Writing a number that can't be stored in what was found.** 999999 into a 1-byte value, 1.5 into a whole number. Refused, nothing written. Pinned in Tasks 8 and 9.

## Not in this plan

- Hunting with no number to type (change-only, from a snapshot). Spec 6.4 puts it in scope for the product. It needs a multi-gigabyte snapshot store and gets its own plan (stage 1b) before stage 2. Until then the dead-end message does not offer it.
- The app, the agent, structure discovery, trainers, freezing a value, the overlay, the macOS back end, launching games. Later stages.
- Working out which bottle a game is in. `--bottle` is required.

## File Structure

```
Makefile                         build, test, integration and bench entry points
.gitignore
docs/dev.md                      how to set up, build and test
docs/verification/disco-elysium.md   real-game log (Task 13)
third_party/doctest/doctest.h
third_party/nlohmann/json.hpp
third_party/README.md            versions and licences of vendored headers
dune-project
helper/CMakeLists.txt
helper/cmake/mingw-w64-x86_64.cmake, mingw-w64-i686.cmake
helper/engine/types.hpp          ValueType, widths, GroupSpec, Pred, EngineError
helper/engine/pred.hpp .cpp      matches()
helper/engine/block_scan.hpp .cpp  scan one buffer for many groups
helper/engine/posset.hpp .cpp    PositionSet: compact candidate positions
helper/engine/memory.hpp         ProcessMemory, Region
helper/engine/engine.hpp .cpp    Engine: scan, narrow, peek, read, write
helper/proto/platform.hpp        Platform: processes, modules, open
helper/proto/handler.hpp .cpp    JSON line in, JSON line out
helper/win/win_memory.cpp        ProcessMemory over Win32
helper/win/win_platform.cpp      Platform over Toolhelp
helper/win/main.cpp              TCP client loop
helper/test/fake_memory.hpp, fake_platform.hpp, test_*.cpp, bench.cpp
tests/target/target.cpp          stand-in target (built 64-bit and 32-bit)
core/lib/value.ml .mli           parse what the user typed
core/lib/interp.ml .mli          interpretations: plan, predicates, encode, describe
core/lib/helper_api.ml           types shared by wire, session and fakes
core/lib/wire.ml .mli            JSON codec for the helper protocol
core/lib/anticheat.ml .mli       refuse list
core/lib/session.ml .mli         the hunt
core/lib/render.ml .mli          user-facing text
core/lib/crossover.ml .mli       find CrossOver, bottles, wine command line
core/lib/helper_conn.ml .mli     launch helper, accept, token, RPC
core/lib/daemon.ml .mli          Unix socket server, dispatch, event log
core/lib/client.ml .mli          connect, auto-start, call
core/bin/main.ml                 argv to client calls
core/test/*.ml                   unit tests, fake helper
core/integration/it_main.ml, target_ctl.ml   bottle tests
bin/                             build output, gitignored
```

## Protocols (shared by several tasks)

**Helper protocol.** Newline-delimited JSON over TCP. The helper connects to `127.0.0.1:<port>` and first sends `{"hello":{"token":"<token>","version":1,"pid":<n>,"arch":"x86_64"}}`. Then the core sends requests `{"id":<n>,"op":"<op>",...}` and the helper answers each with `{"id":<n>,"ok":true,...}` or `{"id":<n>,"ok":false,"error":{"code":"<code>","message":"<text>"}}`.

| op | request fields | reply fields |
| --- | --- | --- |
| `ps` | | `processes:[{pid,name}]` |
| `modules` | `pid` | `modules:[name]` (never empty: a list that cannot be read is the error `modules_failed`) |
| `attach` | `pid` | `pid, name, bits` (32 or 64) |
| `scan` | `groups:[G], keep:bool, budget_mb?` | `groups:[I], stats:S` |
| `narrow` | `groups:[{id,pred:P}], atomic:bool` | `applied:bool, groups:[I], stats:S` |
| `drop` | `ids:[id]` | |
| `peek` | `group, offset, limit` | `items:[{addr,v}]` |
| `read` | `addr, type` | `v` |
| `write` | `addr, type`, and `v` or `bits` | `prev, prev_bits` |
| `status` | | `attached:{pid,name,bits}\|null, groups:[I]` (null, and no groups, once the attached process has exited) |
| `detach`, `bye` | | |

- `G` = `{"id":"i32","type":"i32","align":4,"exclude_align":0,"pred":P}`. `type` is one of `i8 u8 i16 u16 i32 u32 i64 u64 f32 f64`.
- `P` = `{"op":"eq","v":"100"}` (integer types) or `{"op":"range","lo":99.0,"hi":101.0}` (float types, both ends exclusive) or `{"op":"lt_prev"}`, `gt_prev`, `eq_prev`, `ne_prev`.
- `I` = `{"id","count","tracked":bool,"has_values":bool}`. `S` = `{"regions","bytes","unreadable_bytes","lost","ms"}`.
- Values `v` and `prev`: decimal string for integer types (`"100"`, `"-3"`), JSON number for float types. A float that is not finite is the string `"nan"`, `"inf"` or `"-inf"` in `peek`, `read` and `write` replies; a `write` request's `v` is always a finite number. The core decodes `peek` values by the group's type, which it knows. Addresses: hex string (`"0x1f3a0010"`).
- `bits` and `prev_bits`: a value's raw bytes, little-endian, as a lowercase `0x` hex string of two digits per byte of the type (`"0x00000064"` for an `i32` 100, `"0x7fc00000"` for an `f32` nan; a request may leave out leading zeros). A `write` carries `v` or `bits`, not both. `undo` writes back `prev_bits`, so it restores exactly the previous bytes for every type.
- `scan` with `keep:false` replaces every group. With `keep:true` it replaces only the groups it lists.
- Error codes: `bad_request`, `unknown_op`, `not_attached`, `no_such_process`, `access_denied`, `modules_failed`, `no_such_group`, `no_values`, `target_gone`, `read_failed`, `write_failed`, `no_memory` (in any op, when the helper could not get memory; the message is `out of memory`).

**Client protocol.** Newline-delimited JSON over the Unix socket `$MOTH_HOME/core.sock`. Request `{"id":<n>,"actor":"user","cmd":"<cmd>","args":[strings],"opts":{...}}`. Reply `{"id":<n>,"ok":true,"result":<json>,"text":"<rendered>"}` or `{"id":<n>,"ok":false,"error":"<text>"}`. `MOTH_HOME` defaults to `~/Library/Application Support/moth`.

---

### Task 1: Repository and toolchains

**Files:**
- Create: `.gitignore`, `Makefile`, `dune-project`, `docs/dev.md`, `third_party/README.md`, `third_party/doctest/doctest.h`, `third_party/nlohmann/json.hpp`
- Create: `helper/CMakeLists.txt`, `helper/cmake/mingw-w64-x86_64.cmake`, `helper/cmake/mingw-w64-i686.cmake`, `helper/test/test_main.cpp`, `helper/test/test_sanity.cpp`
- Create: `core/lib/dune`, `core/bin/dune`, `core/bin/main.ml`, `core/test/dune`, `core/test/test_sanity.ml`

**Interfaces:**
- Produces: `make setup`, `make build`, `make test` (C++ native unit tests, then `dune test`), `make helper` (Windows build into `bin/`). Variables `WINE ?= $(HOME)/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine` and `MOTH_TEST_BOTTLE ?= uhmm`. OCaml library `moth_core`, executable `moth` copied to `bin/moth`. CMake targets `moth_engine` (static library, portable sources), `moth_tests` (native doctest runner), `moth-helper` (mingw only).

- [ ] **Step 1: Initialise git and ignore build output**

`git init` on branch `main`. `.gitignore`: `_build/`, `_opam/`, `build/`, `bin/`, `.superpowers/`, `.DS_Store`.

- [ ] **Step 2: Set up OCaml without touching shell profiles**

Run: `opam init --bare --no-setup -y && opam switch create . 5.5.2 --no-install -y && opam install -y dune yojson alcotest`
If opam does not offer 5.5.2, use the newest 5.x it offers and record the version in `docs/dev.md`. Every OCaml command in the Makefile goes through `opam exec --`.
Expected: `opam exec -- ocaml -version` prints a 5.x version.

- [ ] **Step 3: Vendor the two headers**

Take `doctest/doctest.h` from the latest doctest release and `single_include/nlohmann/json.hpp` from the latest nlohmann/json release. Record tag, URL and licence (both MIT) in `third_party/README.md`.

- [ ] **Step 4: Write one failing sanity test per side**

```cpp
// helper/test/test_sanity.cpp
TEST_CASE("toolchain") { CHECK(sizeof(void*) == 8); CHECK(nlohmann::json::parse("{\"a\":1}")["a"] == 1); }
```
```ocaml
(* core/test/test_sanity.ml *)
let () = Alcotest.run "sanity" ["yojson", [Alcotest.test_case "parse" `Quick (fun () ->
  Alcotest.(check string) "a" "1" (Yojson.Safe.to_string (Yojson.Safe.Util.member "a" (Yojson.Safe.from_string {|{"a":1}|}))))]]
```

- [ ] **Step 5: Run `make test` and see it fail** (no build files yet)

- [ ] **Step 6: Write the build files**

CMake: C++20, warnings on (`-Wall -Wextra`), include `third_party`. Native configure in `build/native`, mingw in `build/win64` and `build/win32` using the toolchain files, mingw link flags `-static`. `moth_engine` starts as an empty static library with one placeholder source. dune-project `(lang dune 3.0)`, library `moth_core` in `core/lib` depending on `yojson unix`, executable `moth` in `core/bin` printing `moth: nothing here yet` and exiting 2.

- [ ] **Step 7: Run `make test` and `make helper`**

Expected: doctest reports all passed, `dune test` exits 0, and `make helper` produces nothing yet without error.

- [ ] **Step 8: Write `docs/dev.md`** (setup, build, test commands, the chosen OCaml version) **and commit**

```bash
git add -A && git commit -m "scaffold: toolchains, vendored headers, design and plan"
```

---

### Task 2: Scanning one buffer

**Files:**
- Create: `helper/engine/types.hpp`, `helper/engine/pred.hpp`, `helper/engine/pred.cpp`, `helper/engine/block_scan.hpp`, `helper/engine/block_scan.cpp`
- Test: `helper/test/test_block_scan.cpp`

**Interfaces:**
- Produces (namespace `moth`):
```cpp
enum class ValueType : uint8_t { I8, U8, I16, U16, I32, U32, I64, U64, F32, F64 };
size_t width_of(ValueType);                 // 1,1,2,2,4,4,8,8,4,8
bool is_float(ValueType); bool is_signed(ValueType);
bool parse_type(std::string_view, ValueType&);   // "i32" etc.
struct Pred { enum Op { Eq, Range, LtPrev, GtPrev, EqPrev, NePrev } op; uint64_t bits = 0; double lo = 0, hi = 0; };
struct GroupSpec { std::string id; ValueType type; uint32_t align; uint32_t exclude_align; Pred pred; };
struct EngineError : std::runtime_error { std::string code; EngineError(std::string code, std::string msg); };
// cur and prev hold the value's bytes little-endian in the low bits.
bool matches(const Pred&, ValueType, uint64_t cur, uint64_t prev);
// For each group, append ascending offsets o that match, where:
//   o < limit, o + width <= len, (base+o) % align == 0,
//   and exclude_align == 0 or (base+o) % exclude_align != 0.
void scan_block(const uint8_t* data, size_t len, size_t limit, uint64_t base,
                std::span<const GroupSpec> groups, std::vector<std::vector<uint32_t>>& out);
```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("aligned and unaligned i32") {
  Buf b(16); b.put<int32_t>(4, 100); b.put<int32_t>(9, 100);          // helper: zeroed buffer with typed puts
  auto r = run(b, 16, 0, {G("a", I32, 4, 0, eq(100)), G("u", I32, 1, 4, eq(100))});
  CHECK(r[0] == V{4}); CHECK(r[1] == V{9});
}
TEST_CASE("limit and length") {
  Buf b(16); b.put<int32_t>(10, 100); b.put<int32_t>(12, 100);
  CHECK(run(b, 12, 0, {G("u", I32, 1, 0, eq(100))})[0] == V{10});      // 10 starts before limit, 12 does not
  Buf c(16); c.put<int16_t>(14, 100);
  CHECK(run(c, 16, 0, {G("a", I32, 1, 0, eq(100))})[0].empty());       // 14+4 > len, never read past the end
}
TEST_CASE("alignment is by absolute address") {
  Buf b(16); b.put<int32_t>(2, 100); b.put<int32_t>(8, 100);
  CHECK(run(b, 16, 0x1002, {G("a", I32, 4, 0, eq(100))})[0] == V{2});  // 0x1004 aligned, 0x100a not
}
TEST_CASE("float range is exclusive and ignores nan") {
  Buf b(24); float v[] = {99.0f, 99.5f, 100.0f, 100.9f, 101.0f, NAN};
  for (int i = 0; i < 6; i++) b.put<float>(i * 4, v[i]);
  CHECK(run(b, 24, 0, {G("f", F32, 4, 0, range(99, 101))})[0] == V{4, 8, 12});
}
TEST_CASE("other widths") {
  Buf b(32); b.put<uint8_t>(3, 30); b.put<int16_t>(6, -5); b.put<int64_t>(8, 100000); b.put<double>(16, 85.3);
  auto r = run(b, 32, 0, {G("b", U8, 1, 0, eq(30)), G("s", I16, 2, 0, eq(0xFFFB)),
                          G("q", I64, 8, 0, eq(100000)), G("d", F64, 8, 0, range(84, 86))});
  CHECK(r[0] == V{3}); CHECK(r[1] == V{6}); CHECK(r[2] == V{8}); CHECK(r[3] == V{16});
}
TEST_CASE("matches with previous value") {
  CHECK(matches({Pred::LtPrev}, ValueType::I32, bits<int32_t>(-1), bits<int32_t>(5)));   // signed compare
  CHECK_FALSE(matches({Pred::LtPrev}, ValueType::U32, bits<int32_t>(-1), bits<int32_t>(5)));
  CHECK(matches({Pred::GtPrev}, ValueType::F32, bits(2.5f), bits(1.0f)));
  CHECK_FALSE(matches({Pred::GtPrev}, ValueType::F32, bits(NAN), bits(1.0f)));
  CHECK(matches({Pred::EqPrev}, ValueType::F64, bits(1.5), bits(1.5)));
  CHECK(matches({Pred::NePrev}, ValueType::U8, 3, 4));
}
```

`Buf`, `run`, `G`, `eq`, `range`, `V` and `bits` are small helpers local to the test files (a zeroed byte buffer with typed `put`, a wrapper that calls `scan_block` and returns the per-group offsets, and constructors for `GroupSpec` and `Pred`). Put the shared ones in `helper/test/test_util.hpp`; later tasks reuse them.

- [ ] **Step 2: Run `make test`** — expect compile failure, `scan_block` undefined.
- [ ] **Step 3: Implement the files above.** `Eq` compares the low `width` bytes of memory with the low `width` bytes of `pred.bits`. One tight loop per group over the buffer, so each loop stays simple enough to vectorise. `scan_block` rejects previous-value predicates with `EngineError{"bad_request"}`.
- [ ] **Step 4: Run `make test`** — expect pass.
- [ ] **Step 5: Commit** — `git commit -m "engine: scan a buffer for many typed predicates"`

---

### Task 3: Candidate storage and the first scan

**Files:**
- Create: `helper/engine/memory.hpp`, `helper/engine/posset.hpp`, `helper/engine/posset.cpp`, `helper/engine/engine.hpp`, `helper/engine/engine.cpp`, `helper/test/fake_memory.hpp`
- Test: `helper/test/test_engine_scan.cpp`

**Interfaces:**
- Consumes: Task 2's `GroupSpec`, `scan_block`.
- Produces:
```cpp
struct Region { uint64_t base; uint64_t size; };
class ProcessMemory { public: virtual ~ProcessMemory() = default;
  virtual std::vector<Region> writable_regions() = 0;
  virtual size_t read(uint64_t addr, void* out, size_t len) = 0;     // bytes read, 0 on failure
  virtual bool write(uint64_t addr, const void* data, size_t len) = 0;
  virtual bool alive() = 0; };
struct GroupInfo { std::string id; uint64_t count; bool tracked; bool has_values; };
struct ScanStats { uint64_t regions = 0, bytes = 0, unreadable_bytes = 0, lost = 0; double ms = 0; };
struct EngineConfig { uint64_t budget_bytes = 1536ull << 20; uint64_t value_cap = 2'000'000;
                      size_t block_size = 16u << 20; unsigned workers = 1; };
struct Item { uint64_t addr; uint64_t bits; };
class Engine { public:
  explicit Engine(ProcessMemory&, EngineConfig = {});
  std::vector<GroupInfo> scan(const std::vector<GroupSpec>&, bool keep, ScanStats&);
  std::vector<Item> peek(const std::string& id, uint64_t offset, uint64_t limit);  // reads current values
  std::vector<GroupInfo> groups() const;
  uint64_t storage_bytes() const; };
// fake_memory.hpp
class FakeMemory : public ProcessMemory { public:
  void add_region(uint64_t base, size_t size);   void remove_region(uint64_t base);
  void set_unreadable(uint64_t base, bool);      void set_readonly(uint64_t base, bool);
  void set_alive(bool);                          template <class T> void poke(uint64_t addr, T v);
  template <class T> T peek(uint64_t addr);      size_t read_calls = 0; uint64_t bytes_read = 0; };
```
- Storage rules the tests rely on:
  - Each region is read in blocks of `block_size`. A block that is followed by more of the same region is read with up to 7 extra bytes, and `scan_block` is called with `limit` = the block's own size, so a value that straddles two blocks is found once.
  - A `PositionSet` keeps, per block, either a sorted array of 32-bit offsets or a bitmap with one bit per `align` step, whichever is smaller.
  - A group holds last-seen values only while `count <= value_cap`. `has_values` reports it.
  - After a scan, if `storage_bytes() > budget_bytes`, groups are untracked from the end of the request's group list backwards until it fits. An untracked group keeps its true `count`, frees its storage, and reports `tracked:false`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("finds values across regions in address order") {
  FakeMemory m; m.add_region(0x10000, 4096); m.add_region(0x30000, 4096);
  m.poke<int32_t>(0x30010, 100); m.poke<int32_t>(0x10008, 100);
  Engine e(m); ScanStats s; auto g = e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  CHECK(g[0].count == 2); CHECK(g[0].tracked); CHECK(g[0].has_values);
  auto p = e.peek("a", 0, 10); CHECK(p[0].addr == 0x10008); CHECK(p[1].addr == 0x30010); CHECK(p[0].bits == 100);
  CHECK(s.regions == 2); CHECK(s.bytes == 8192);
}
TEST_CASE("value straddling a block boundary is found once") {
  FakeMemory m; m.add_region(0x10000, 8192); m.poke<int32_t>(0x10000 + 4094, 100);
  Engine e(m, {.block_size = 4096}); ScanStats s;
  CHECK(e.scan({G("u", I32, 1, 0, eq(100))}, false, s)[0].count == 1);
}
TEST_CASE("unreadable region is counted, not fatal") {
  FakeMemory m; m.add_region(0x10000, 4096); m.set_unreadable(0x10000, true);
  Engine e(m); ScanStats s; CHECK(e.scan({G("a", I32, 4, 0, eq(100))}, false, s)[0].count == 0);
  CHECK(s.unreadable_bytes == 4096);
}
TEST_CASE("values are kept only up to value_cap") {
  FakeMemory m; m.add_region(0x10000, 4096); for (int i = 0; i < 5; i++) m.poke<int32_t>(0x10000 + i * 4, 100);
  Engine e(m, {.value_cap = 4}); ScanStats s; auto g = e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  CHECK(g[0].count == 5); CHECK(g[0].tracked); CHECK_FALSE(g[0].has_values);
}
TEST_CASE("dense group is stored as a bitmap") {
  FakeMemory m; m.add_region(0x100000, 1 << 20);                       // 1 MiB of zeros
  Engine e(m, {.value_cap = 0}); ScanStats s; auto g = e.scan({G("z", U8, 1, 0, eq(0))}, false, s);
  CHECK(g[0].count == (1u << 20)); CHECK(e.storage_bytes() <= 140'000);
}
TEST_CASE("over budget, the last groups become untracked") {
  FakeMemory m; m.add_region(0x100000, 1 << 20);
  Engine e(m, {.budget_bytes = 150'000, .value_cap = 0}); ScanStats s;
  auto g = e.scan({G("z", U8, 1, 0, eq(0)), G("y", I16, 2, 0, eq(0))}, false, s);
  CHECK(g[0].tracked); CHECK_FALSE(g[1].tracked); CHECK(g[1].count == (1u << 19));
}
TEST_CASE("keep replaces only the listed groups") {
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<int32_t>(0x10000, 100); m.poke<int32_t>(0x10004, 7);
  Engine e(m); ScanStats s; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  e.scan({G("b", I32, 4, 0, eq(7))}, true, s); CHECK(e.groups().size() == 2);
  e.scan({G("c", I32, 4, 0, eq(7))}, false, s); CHECK(e.groups().size() == 1);
}
```

- [ ] **Step 2: Run `make test`** — expect compile failure.
- [ ] **Step 3: Implement.** Single-threaded here; `workers` is honoured in Task 5.
- [ ] **Step 4: Run `make test`** — expect pass.
- [ ] **Step 5: Commit** — `git commit -m "engine: compact candidate storage and first scan"`

---

### Task 4: Narrowing, reading and writing

**Files:**
- Modify: `helper/engine/engine.hpp`, `helper/engine/engine.cpp`
- Test: `helper/test/test_engine_narrow.cpp`

**Interfaces:**
- Produces (added to `Engine`):
```cpp
struct NarrowItem { std::string id; Pred pred; };
// Returns applied. With atomic=true, if the listed groups would be left with zero matches in total,
// nothing changes and it returns false. Groups not listed are untouched.
bool narrow(const std::vector<NarrowItem>&, bool atomic, std::vector<GroupInfo>& out, ScanStats&);
void drop(const std::vector<std::string>& ids);
bool read_value(uint64_t addr, ValueType, uint64_t& bits);
bool write_value(uint64_t addr, ValueType, uint64_t bits, uint64_t& prev_bits);
```
- Errors thrown as `EngineError`: `no_such_group`, `no_values` (previous-value predicate on a group with `has_values == false`), `target_gone` (`alive()` is false).
- Read strategy for narrowing: walk a group's candidates in address order and read them in runs. A run ends when the next candidate is more than 64 KiB past the previous one, or at a block boundary. One `read` call per run. If a run's read fails, its candidates are dropped and counted in `stats.lost`.
- After an applied narrow, a group whose count is now `<= value_cap` has values (captured from the bytes just read), and its stored values are the current ones.

- [ ] **Step 1: Write the failing tests**

```cpp
static void three(FakeMemory& m) { m.add_region(0x10000, 4096); for (int i = 0; i < 3; i++) m.poke<int32_t>(0x10000 + i * 16, 100); }
TEST_CASE("narrow by a new number") {
  FakeMemory m; three(m); Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s); m.poke<int32_t>(0x10010, 85);
  CHECK(e.narrow({{"a", eq(85)}}, true, g, s)); CHECK(g[0].count == 1); CHECK(e.peek("a", 0, 9)[0].addr == 0x10010);
}
TEST_CASE("atomic narrow that would empty everything changes nothing") {
  FakeMemory m; three(m); Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  CHECK_FALSE(e.narrow({{"a", eq(77)}}, true, g, s)); CHECK(e.groups()[0].count == 3);
  CHECK(e.narrow({{"a", eq(77)}}, false, g, s)); CHECK(e.groups()[0].count == 0);
}
TEST_CASE("relations compare with the last applied step") {
  FakeMemory m; three(m); Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.poke<int32_t>(0x10000, 90); m.poke<int32_t>(0x10010, 110);
  CHECK(e.narrow({{"a", {Pred::NePrev}}}, true, g, s)); CHECK(g[0].count == 2);
  m.poke<int32_t>(0x10000, 80);                                         // down again from 90
  CHECK(e.narrow({{"a", {Pred::LtPrev}}}, true, g, s)); CHECK(g[0].count == 1);
  CHECK(e.peek("a", 0, 9)[0].addr == 0x10000);
}
TEST_CASE("relation without stored values is refused, then allowed once small") {
  FakeMemory m; three(m); Engine e(m, {.value_cap = 2}); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  CHECK_THROWS_WITH_AS(e.narrow({{"a", {Pred::LtPrev}}}, true, g, s), doctest::Contains("no_values"), EngineError);
  m.poke<int32_t>(0x10000, 85); m.poke<int32_t>(0x10010, 85);
  CHECK(e.narrow({{"a", eq(85)}}, true, g, s)); CHECK(g[0].has_values);
  m.poke<int32_t>(0x10000, 60); CHECK(e.narrow({{"a", {Pred::LtPrev}}}, true, g, s)); CHECK(g[0].count == 1);
}
TEST_CASE("freed memory loses candidates instead of failing") {
  FakeMemory m; three(m); m.add_region(0x50000, 4096); m.poke<int32_t>(0x50000, 100);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.remove_region(0x50000); CHECK(e.narrow({{"a", eq(100)}}, true, g, s)); CHECK(g[0].count == 3); CHECK(s.lost == 1);
}
TEST_CASE("sparse candidates are read in runs, not whole blocks") {
  FakeMemory m; m.add_region(0x1000000, 12u << 20);
  m.poke<int32_t>(0x1000000, 100); m.poke<int32_t>(0x1000064, 100); m.poke<int32_t>(0x1000000 + (10u << 20), 100);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.read_calls = 0; m.bytes_read = 0; e.narrow({{"a", eq(100)}}, true, g, s);
  CHECK(m.read_calls == 2); CHECK(m.bytes_read < (1u << 20));
}
TEST_CASE("the game closing is reported") {
  FakeMemory m; three(m); Engine e(m); ScanStats s; std::vector<GroupInfo> g; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  m.set_alive(false); CHECK_THROWS_WITH_AS(e.narrow({{"a", eq(1)}}, true, g, s), doctest::Contains("target_gone"), EngineError);
}
TEST_CASE("read, write and peek paging") {
  FakeMemory m; three(m); Engine e(m); ScanStats s; e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  uint64_t prev = 0, cur = 0; CHECK(e.write_value(0x10000, ValueType::I32, 999, prev)); CHECK(prev == 100);
  CHECK(e.read_value(0x10000, ValueType::I32, cur)); CHECK(cur == 999);
  CHECK(e.peek("a", 1, 1).size() == 1); CHECK(e.peek("a", 1, 1)[0].addr == 0x10010);
  m.set_readonly(0x10000, true); CHECK_FALSE(e.write_value(0x10000, ValueType::I32, 1, prev));
  e.drop({"a"}); CHECK(e.groups().empty());
}
```

- [ ] **Step 2: Run `make test`** — expect compile failure.
- [ ] **Step 3: Implement.** Atomic narrowing builds the new sets beside the old ones and swaps only if the total is above zero.
- [ ] **Step 4: Run `make test`** — expect pass.
- [ ] **Step 5: Commit** — `git commit -m "engine: narrow, read and write"`

---

### Task 5: Scanning blocks in parallel

**Files:**
- Modify: `helper/engine/engine.cpp`, `helper/CMakeLists.txt`
- Create: `helper/test/bench.cpp`
- Test: `helper/test/test_engine_parallel.cpp`

**Interfaces:**
- Consumes: `EngineConfig::workers`. `0` means `max(1, min(8, hardware_concurrency() - 1))`.
- Produces: identical results for any worker count. `unsigned Engine::workers_used() const` (the resolved count). CMake target `moth_bench` and `make bench-native`, printing `scan: <x> s/GB, narrow: <y> ms` for a 256 MiB seeded `FakeMemory` with the twelve groups a whole number produces (Task 8's list for `100`).

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("worker count does not change results") {
  FakeMemory m; m.add_region(0x10000000, 64u << 20); fill_xorshift(m, 0x10000000, 64u << 20, 0x6d6f7468);
  std::vector<GroupSpec> gs = {G("b", U8, 1, 0, eq(30)), G("a", I32, 4, 0, eq(100)), G("f", F32, 4, 0, range(99, 101))};
  auto run = [&](unsigned w) { Engine e(m, {.block_size = 1u << 20, .workers = w}); ScanStats s; e.scan(gs, false, s);
    std::vector<std::vector<Item>> out; for (auto& g : gs) out.push_back(e.peek(g.id, 0, 1u << 30)); return out; };
  auto one = run(1), four = run(4), autow = run(0);
  CHECK(addrs(one) == addrs(four)); CHECK(addrs(one) == addrs(autow)); CHECK(one[0].size() > 200'000);
  Engine e4(m, {.workers = 4}); CHECK(e4.workers_used() == 4);
  Engine e0(m, {.workers = 0}); CHECK(e0.workers_used() >= 1); CHECK(e0.workers_used() <= 8);
}
```

- [ ] **Step 2: Run `make test`** — expect compile failure, `workers_used` undefined.
- [ ] **Step 3: Implement.** The calling thread reads blocks in address order and hands them to workers through a bounded queue of at most 4 blocks. Workers run `scan_block`. Per-block results are stitched in block order. Reading stays on one thread.
- [ ] **Step 4: Run `make test` and `make bench-native`** — expect pass and a printed figure. Record the figure in `docs/dev.md`.
- [ ] **Step 5: Commit** — `git commit -m "engine: scan blocks on worker threads"`

---

### Task 6: The helper's protocol handler

**Files:**
- Create: `helper/proto/platform.hpp`, `helper/proto/handler.hpp`, `helper/proto/handler.cpp`, `helper/test/fake_platform.hpp`
- Test: `helper/test/test_handler.cpp`

**Interfaces:**
- Consumes: `Engine`, `ProcessMemory`.
- Produces:
```cpp
struct ProcInfo { uint32_t pid; std::string name; int bits; };
class Platform { public: virtual ~Platform() = default;
  virtual std::vector<ProcInfo> processes() = 0;
  virtual std::vector<std::string> modules(uint32_t pid) = 0;
  // Throws EngineError{"no_such_process"} or {"access_denied"}.
  virtual std::unique_ptr<ProcessMemory> open(uint32_t pid, ProcInfo& out) = 0; };
class Handler { public:
  Handler(Platform&, EngineConfig = {});
  std::string handle_line(const std::string& line);   // one JSON reply, no trailing newline
  bool wants_exit() const; };                         // true after "bye"
```
- Follows the helper protocol table at the top of this plan exactly. `budget_mb` in a `scan` request overrides the configured budget for that hunt.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("ps, attach, scan, narrow, peek, write round trip") {
  FakePlatform p; p.add(208, "disco.exe", 64); p.mem(208).add_region(0x10000, 4096); p.mem(208).poke<int32_t>(0x10008, 100);
  Handler h(p);
  CHECK(J(h.handle_line(R"({"id":1,"op":"ps"})"))["processes"][0]["name"] == "disco.exe");
  CHECK(J(h.handle_line(R"({"id":2,"op":"attach","pid":208})"))["bits"] == 64);
  auto sc = J(h.handle_line(R"({"id":3,"op":"scan","keep":false,"groups":[
     {"id":"i32","type":"i32","align":4,"exclude_align":0,"pred":{"op":"eq","v":"100"}},
     {"id":"f32","type":"f32","align":4,"exclude_align":0,"pred":{"op":"range","lo":99.0,"hi":101.0}}]})"));
  CHECK(sc["ok"] == true); CHECK(sc["id"] == 3); CHECK(sc["groups"][0]["count"] == 1); CHECK(sc["stats"]["regions"] == 1);
  auto pk = J(h.handle_line(R"({"id":4,"op":"peek","offset":0,"limit":5})"));              // group missing
  CHECK(pk["ok"] == false); CHECK(pk["error"]["code"] == "bad_request");
  pk = J(h.handle_line(R"({"id":5,"op":"peek","group":"i32","offset":0,"limit":5})"));
  CHECK(pk["items"][0]["addr"] == "0x10008"); CHECK(pk["items"][0]["v"] == "100");
  auto wr = J(h.handle_line(R"({"id":6,"op":"write","addr":"0x10008","type":"i32","v":"999"})"));
  CHECK(wr["prev"] == "100"); CHECK(p.mem(208).peek<int32_t>(0x10008) == 999);
  auto nr = J(h.handle_line(R"({"id":7,"op":"narrow","atomic":true,"groups":[{"id":"i32","pred":{"op":"eq","v":"5"}}]})"));
  CHECK(nr["applied"] == false);
}
TEST_CASE("errors") {
  FakePlatform p; Handler h(p);
  CHECK(J(h.handle_line("not json"))["error"]["code"] == "bad_request");
  CHECK(J(h.handle_line(R"({"id":1,"op":"dance"})"))["error"]["code"] == "unknown_op");
  CHECK(J(h.handle_line(R"({"id":2,"op":"scan","keep":false,"groups":[]})"))["error"]["code"] == "not_attached");
  CHECK(J(h.handle_line(R"({"id":3,"op":"attach","pid":9})"))["error"]["code"] == "no_such_process");
}
TEST_CASE("float values, negative integers and bye") {
  FakePlatform p; p.add(1, "t.exe", 64); p.mem(1).add_region(0x10000, 4096); p.mem(1).poke<float>(0x10000, 85.5f); p.mem(1).poke<int16_t>(0x10010, -5);
  Handler h(p); h.handle_line(R"({"id":1,"op":"attach","pid":1})");
  CHECK(J(h.handle_line(R"({"id":2,"op":"read","addr":"0x10000","type":"f32"})"))["v"] == doctest::Approx(85.5));
  CHECK(J(h.handle_line(R"({"id":3,"op":"read","addr":"0x10010","type":"i16"})"))["v"] == "-5");
  CHECK(J(h.handle_line(R"({"id":4,"op":"write","addr":"0x10010","type":"i16","v":"70000"})"))["error"]["code"] == "bad_request");
  h.handle_line(R"({"id":5,"op":"bye"})"); CHECK(h.wants_exit());
}
```
`J` parses a reply with nlohmann/json. `FakePlatform` (in `fake_platform.hpp`) holds one `FakeMemory` per added process.

- [ ] **Step 2: Run `make test`** — expect compile failure.
- [ ] **Step 3: Implement.** `EngineError` codes map straight to protocol error codes. Any other exception becomes `bad_request` with its message.
- [ ] **Step 4: Run `make test`** — expect pass.
- [ ] **Step 5: Commit** — `git commit -m "helper: json protocol handler"`

---

### Task 7: The Windows helper and the stand-in target

**Files:**
- Create: `helper/win/win_memory.cpp`, `helper/win/win_platform.cpp`, `helper/win/main.cpp`, `tests/target/target.cpp`
- Modify: `helper/CMakeLists.txt`, `Makefile`

**Interfaces:**
- Consumes: `Handler`, `Platform`, `ProcessMemory`.
- Produces:
  - `bin/moth-helper.exe <port> <token>`: connects to `127.0.0.1:<port>` with `TCP_NODELAY`, sends the hello line, then answers one line at a time until the socket closes or `bye`. Exits 0 on a clean end, 2 if it cannot connect.
  - Writable regions: `MEM_COMMIT`, protection one of `PAGE_READWRITE`, `PAGE_WRITECOPY`, `PAGE_EXECUTE_READWRITE`, `PAGE_EXECUTE_WRITECOPY`, without `PAGE_GUARD` or `PAGE_NOACCESS`. Private, mapped and image memory are all included.
  - `alive()`: the process handle is not signalled.
  - `bits`: 32 when `IsWow64Process` says so, otherwise 64. Process and module names come from Toolhelp snapshots (`TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32` for modules).
  - `bin/moth-target.exe <port> [--filler-mb N]` and `bin/moth-target32.exe` (same source, i686 build): connects to `127.0.0.1:<port>`, sends one manifest line `{"vars":[{"name","type","addr","value"}]}`, then obeys `set <name> <value>` (reply `ok`), `get <name>` (reply the value), `quit`.
  - Target variables and starting values: `health` f32 `100.0`; `health_copy` i32 `100`; `ammo` u8 `30`; `level` i16 `7`; `xp` i64 `100000`; `morale` f64 `85.3`; `money_cents` i32 `13250`; `gems` i32 `4242` placed at an odd address inside a packed struct; `lives` i32 `3`. All live in one heap block so they are not adjacent to the filler.
  - `--filler-mb N`: one `VirtualAlloc` of N MiB, committed read-write, filled from xorshift64 seeded `0x6d6f7468`.
  - `--filler-small-mb N`: one `VirtualAlloc` of N MiB where every 4-byte slot holds a number from 0 to 7 (the same generator, modulo 8). This imitates real game memory, which is full of small numbers.
  - `make helper` builds all three into `bin/`. `make smoke` proves the helper connects from the bottle.

- [ ] **Step 1: Add the `smoke` target and run it to see it fail**

```make
smoke: helper
	@mkdir -p build && rm -f build/smoke.out
	@(printf '{"id":1,"op":"ps"}\n{"id":2,"op":"bye"}\n' | nc -l 127.0.0.1 47113 > build/smoke.out) & \
	 sleep 1; "$(WINE)" --bottle "$(MOTH_TEST_BOTTLE)" --workdir "$(CURDIR)/bin" "$(CURDIR)/bin/moth-helper.exe" 47113 smoketoken >/dev/null 2>&1; wait
	@grep -q '"hello"' build/smoke.out && grep -q 'smoketoken' build/smoke.out && grep -q 'moth-helper.exe' build/smoke.out && echo "smoke ok"
```
Run: `make smoke` — expect failure, no `moth-helper.exe`.

- [ ] **Step 2: Implement the Windows back end, `main.cpp` and the target.** Link `-static -lws2_32`. Read from the socket into a line buffer; a request line may be larger than one `recv`.
- [ ] **Step 3: Run `make smoke`** — expect `smoke ok`. The reply to `ps` lists `moth-helper.exe` itself.
- [ ] **Step 4: Run `make test`** — native tests still pass (the Windows sources are excluded from the native build).
- [ ] **Step 5: Commit** — `git commit -m "helper: windows back end, tcp loop, stand-in target"`

---

### Task 8: What the user typed, and every way it could be stored

**Files:**
- Create: `core/lib/value.ml`, `core/lib/value.mli`, `core/lib/interp.ml`, `core/lib/interp.mli`, `core/lib/helper_api.ml`
- Test: `core/test/test_value.ml`, `core/test/test_interp.ml`

**Interfaces:**
- Produces:
```ocaml
(* helper_api.ml *)
type value = Vint of int64 | Vfloat of float
type pred = Eq of int64 | Range of float * float | Lt_prev | Gt_prev | Eq_prev | Ne_prev
type group_spec = { id : string; vtype : string; align : int; exclude_align : int; pred : pred }
type group_info = { id : string; count : int; tracked : bool; has_values : bool }
type stats = { regions : int; bytes : int; unreadable_bytes : int; lost : int; ms : float }
type item = { addr : int64; v : value }
type attached = { pid : int; name : string; bits : int }
type err = { code : string; message : string }
type helper = {
  ps : unit -> ((int * string) list, err) result;
  modules : int -> (string list, err) result;
  attach : int -> (attached, err) result;
  scan : keep:bool -> group_spec list -> (group_info list * stats, err) result;
  narrow : atomic:bool -> (string * pred) list -> (bool * group_info list * stats, err) result;
  drop : string list -> (unit, err) result;
  peek : id:string -> offset:int -> limit:int -> (item list, err) result;
  read : addr:int64 -> vtype:string -> (value, err) result;
  write : addr:int64 -> vtype:string -> value -> (value, err) result;   (* returns the previous value *)
  detach : unit -> (unit, err) result;
  close : unit -> unit;
}
(* value.mli *)
type t = Int of int64 | Dec of { text : string; value : float; decimals : int }
val parse : string -> (t, string) result
val to_float : t -> float
(* interp.mli *)
type tier = Main | Wide
type encoding = Plain | Scaled of int | Fraction
type t = { id : string; vtype : string; align : int; exclude_align : int; tier : tier; encoding : encoding }
val plan : Value.t -> t list                       (* priority order; also the order sent to the helper *)
val pred_for : t -> Value.t -> Helper_api.pred option
val encode : t -> Value.t -> (Helper_api.value, string) result
val decode : t -> Helper_api.value -> string       (* what to show the user *)
val describe : t -> string
```
- `Value.parse` trims spaces, removes `,` and `_`, accepts an optional leading `+` or `-`, digits, and an optional `.digits`. Anything else is an error. Integers must fit a signed 64-bit number.
- Plan for a whole number `n`, in this order, each included only if `n` (or the product) fits:
  - Main: `i32` (or `u32` when `n > 2147483647`), `f32` range `(n-1, n+1)`, `i16` (or `u16` when `32767 < n <= 65535`), `i64`, `f64` range `(n-1, n+1)`. Natural alignment (4, 4, 2, 8, 8).
  - Wide: `u8` (or `i8` when `n < 0`) align 1; `i32x100`, `i32x10`, `i32x1000` (align 4, `Scaled`, only when the product fits a signed 32-bit number); `f32pct` (align 4, `Fraction`, range `((n-1)/100, (n+1)/100)`); `i32u` (or `u32u`) align 1 exclude 4; `f32u` align 1 exclude 4.
- Plan for a decimal `d` with `k` decimal places, in this order:
  - Main: `f32` range `(d - 10^-k, d + 10^-k)`, `f64` same range, `i32x<10^k>` and `i64x<10^k>` (`Scaled (10^k)`, value `round (d * 10^k)`).
  - Wide: `f32u`.
- `pred_for` applies the interpretation to a later number. A whole-number interpretation given a decimal returns `None`. A value that doesn't fit the type returns `None`. A float interpretation given a decimal with `k` places uses `(d - 10^-k, d + 10^-k)`.
- `describe`: `i32`/`u32` "whole number"; `i16`/`u16` "whole number (2-byte)"; `i64` "whole number (8-byte)"; `u8`/`i8` "whole number (1-byte)"; `f32` "decimal"; `f64` "decimal (8-byte)"; `i32xN`/`i64xN` "whole number ×N"; `f32pct` "decimal ÷100"; `i32u`/`u32u` "whole number (unaligned)"; `f32u` "decimal (unaligned)".

- [ ] **Step 1: Write the failing tests**

```ocaml
(* test_value.ml *)
let ok s v = Alcotest.(check bool) s true (Value.parse s = Ok v)
let bad s = Alcotest.(check bool) s true (Result.is_error (Value.parse s))
let cases () =
  ok "100" (Int 100L); ok " 1,250 " (Int 1250L); ok "-3" (Int (-3L)); ok "+5" (Int 5L); ok "1_000" (Int 1000L);
  ok "132.50" (Dec { text = "132.50"; value = 132.5; decimals = 2 }); ok "-0.5" (Dec { text = "-0.5"; value = -0.5; decimals = 1 });
  List.iter bad [ ""; "abc"; "85%"; "$5"; "1e5"; "12.3.4"; "."; "5."; "9223372036854775808" ]
(* test_interp.ml *)
let ids v = List.map (fun (i : Interp.t) -> i.id) (Interp.plan v)
let cases () =
  check_ids "100" (ids (Int 100L)) [ "i32"; "f32"; "i16"; "i64"; "f64"; "u8"; "i32x100"; "i32x10"; "i32x1000"; "f32pct"; "i32u"; "f32u" ];
  check_ids "70000" (ids (Int 70000L)) [ "i32"; "f32"; "i64"; "f64"; "i32x100"; "i32x10"; "i32x1000"; "f32pct"; "i32u"; "f32u" ];
  check_ids "40000" (ids (Int 40000L)) [ "i32"; "f32"; "u16"; "i64"; "f64"; "i32x100"; "i32x10"; "i32x1000"; "f32pct"; "i32u"; "f32u" ];
  check_ids "3e9" (ids (Int 3_000_000_000L)) [ "u32"; "f32"; "i64"; "f64"; "f32pct"; "u32u"; "f32u" ];
  check_ids "-3" (ids (Int (-3L))) [ "i32"; "f32"; "i16"; "i64"; "f64"; "i8"; "i32x100"; "i32x10"; "i32x1000"; "f32pct"; "i32u"; "f32u" ];
  check_ids "132.50" (ids (dec "132.50")) [ "f32"; "f64"; "i32x100"; "i64x100"; "f32u" ];
  let i id v = List.find (fun (x : Interp.t) -> x.id = id) (Interp.plan v) in
  check_tier "u8 is wide" Wide (i "u8" (Int 100L)).tier; check_tier "i32x100 of a decimal is main" Main (i "i32x100" (dec "132.50")).tier;
  check_int "i32u excludes aligned" 4 (i "i32u" (Int 100L)).exclude_align;
  check_pred "eq" (Some (Eq 100L)) (Interp.pred_for (i "i32" (Int 100L)) (Int 100L));
  check_pred "float range" (Some (Range (99., 101.))) (Interp.pred_for (i "f32" (Int 100L)) (Int 100L));
  check_pred "scaled" (Some (Eq 8500L)) (Interp.pred_for (i "i32x100" (Int 100L)) (Int 85L));
  check_pred "pct" (Some (Range (0.84, 0.86))) (Interp.pred_for (i "f32pct" (Int 100L)) (Int 85L));
  check_pred "cents" (Some (Eq 13250L)) (Interp.pred_for (i "i32x100" (dec "132.50")) (dec "132.50"));
  check_pred "decimal range" (Some (Range (132.49, 132.51))) (Interp.pred_for (i "f32" (dec "132.50")) (dec "132.50"));
  check_pred "whole given decimal" None (Interp.pred_for (i "i32" (Int 100L)) (dec "85.5"));
  check_pred "does not fit" None (Interp.pred_for (i "u8" (Int 100L)) (Int 300L));
  check_enc "int" (Ok (Vint 999L)) (Interp.encode (i "i32" (Int 100L)) (Int 999L));
  check_enc "float" (Ok (Vfloat 999.)) (Interp.encode (i "f32" (Int 100L)) (Int 999L));
  check_enc "cents" (Ok (Vint 99999L)) (Interp.encode (i "i32x100" (dec "132.50")) (dec "999.99"));
  check_enc "pct" (Ok (Vfloat 0.5)) (Interp.encode (i "f32pct" (Int 100L)) (Int 50L));
  check_err "too big" "999999 doesn't fit in a whole number (1-byte)" (Interp.encode (i "u8" (Int 100L)) (Int 999999L));
  check_err "fraction" "this value is a whole number, and 1.5 has a decimal part" (Interp.encode (i "i32" (Int 100L)) (dec "1.5"));
  check_str "decode cents" "132.50" (Interp.decode (i "i32x100" (dec "132.50")) (Vint 13250L));
  check_str "decode float" "85.3" (Interp.decode (i "f32" (Int 100L)) (Vfloat 85.3));
  check_str "describe" "whole number ×100" (Interp.describe (i "i32x100" (Int 100L)))
```
Range checks compare floats with a tolerance of `1e-9`. `decode` prints floats with up to 6 significant digits and no trailing zeros, and scaled values with the scale's number of decimal places.

- [ ] **Step 2: Run `opam exec -- dune test`** — expect compile failure.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run `opam exec -- dune test`** — expect pass.
- [ ] **Step 5: Commit** — `git commit -m "core: parse typed numbers and plan interpretations"`

---

### Task 9: The hunt

**Files:**
- Create: `core/lib/wire.ml`, `core/lib/wire.mli`, `core/lib/session.ml`, `core/lib/session.mli`, `core/lib/render.ml`, `core/lib/render.mli`, `core/test/fake_helper.ml`
- Test: `core/test/test_wire.ml`, `core/test/test_session.ml`, `core/test/test_render.ml`

**Interfaces:**
- Consumes: `Helper_api`, `Value`, `Interp`.
- Produces:
```ocaml
(* wire.mli: the helper protocol table at the top of this plan *)
val request : id:int -> op:string -> (string * Yojson.Safe.t) list -> string
val group_spec_to_json : Helper_api.group_spec -> Yojson.Safe.t
val pred_to_json : Helper_api.pred -> Yojson.Safe.t
val parse_reply : string -> (int * Yojson.Safe.t, Helper_api.err) result      (* id, body *)
val group_infos : Yojson.Safe.t -> Helper_api.group_info list
val stats : Yojson.Safe.t -> Helper_api.stats
val items : vtype:string -> Yojson.Safe.t -> Helper_api.item list
val value_to_json : Helper_api.value -> Yojson.Safe.t
val value_of_json : vtype:string -> Yojson.Safe.t -> Helper_api.value
(* session.mli *)
type t
type relation = Down | Up | Same | Changed
type note = Fell_back of relation | Showing_wide | Untracked | Lost of int
type outcome = { main : int; wide : int; by_tag : (string * int) list; notes : note list; dead_end : bool; ms : float }
type row = { n : int; addr : int64; interp_id : string; tag : string; shown : string }
val create : Helper_api.helper -> t
val find : t -> string -> (outcome, string) result
val next_number : t -> string -> (outcome, string) result
val next_relation : t -> relation -> (outcome, string) result
val results : t -> all:bool -> (row list, string) result
val set : t -> row:int -> string -> (string, string) result
val undo : t -> (string, string) result
val writes : t -> (int64 * string * Helper_api.value * Helper_api.value) list   (* addr, vtype, previous, new; newest first *)
(* render.mli *)
val outcome : Session.outcome -> string
val rows : Session.row list -> string
val commas : int -> string
```
- Session rules:
  - `find` sends one `scan ~keep:false` with every interpretation from `Interp.plan`, in plan order.
  - `next_number m`: first `narrow ~atomic:true` over tracked groups, each with `Interp.pred_for` on `m` (groups where that is `None` are dropped). Then `scan ~keep:true` for untracked groups. If the narrow was not applied but the rescan found matches, the old tracked groups are dropped. If nothing matched anywhere, fall back: compare `m` with the last number typed and retry `narrow ~atomic:true` with `Lt_prev`, `Gt_prev` or `Eq_prev` over the groups that have values, adding `Fell_back`. If that also finds nothing, `dead_end = true` and nothing has changed.
  - `next_relation`: `narrow ~atomic:true` over groups that have values. Groups without values are left alone. If no group has values: `Error "too many matches to compare by change yet. type the new number instead."` Zero matches is a dead end with nothing changed.
  - `main` and `wide` are the totals of tracked groups in each tier. `Showing_wide` is added when `main = 0` and `wide > 0`. `Untracked` is added when any group is untracked. `Lost n` when the helper's `lost` is above zero.
  - `results`: rows numbered from 1. Groups in plan order, at most 10 rows per group, only groups with `count <= 1000`. Wide groups are included when `all` is true or `Showing_wide` applies.
  - `set` looks the row up in the last `results` listing, encodes with `Interp.encode`, writes, and records the write. Message: `set #<n> to <shown new> (was <shown old>)`. `undo` writes the previous value back: `restored #<n> to <shown old>`.
  - Helper error `target_gone` anywhere: the hunt is cleared and the error is `the game closed. attach again once it's running.`
- Render copy (exact):
  - Counts line: `<total> matches (<count> <tag>, ...)` listing tags with a count above zero, numbers with comma separators. One match: `1 match (...)`.
  - `Fell_back Down`: `no exact match for that number. kept the ones that went down instead.` (`went up`, `stayed the same` for the others.)
  - `Showing_wide`: `nothing matched as a plain number. showing other ways it could be stored.`
  - `Untracked`: `some rarer forms are too common to track yet. they'll be rechecked on your next number.`
  - `Lost n`: `<n> matches disappeared because the game freed that memory.`
  - Dead end: `nothing left that fits. that step was not applied, so you can try a different number.` A dead end prints only this line.
  - Lines are joined with `\n`, counts line first, then notes in the order above.
- `fake_helper.ml`: `Fake_helper.create : unit -> Helper_api.helper * Fake_helper.script`, where the script lets a test queue replies per operation and read back the calls made.

- [ ] **Step 1: Write the failing tests**

```ocaml
(* test_wire.ml *)
check_str "scan request" {|{"id":3,"op":"scan","keep":false,"groups":[{"id":"i32","type":"i32","align":4,"exclude_align":0,"pred":{"op":"eq","v":"100"}}]}|}
  (Wire.request ~id:3 ~op:"scan" [ "keep", `Bool false; "groups", `List [ Wire.group_spec_to_json { id = "i32"; vtype = "i32"; align = 4; exclude_align = 0; pred = Eq 100L } ] ]);
check_bool "error reply" true (Wire.parse_reply {|{"id":4,"ok":false,"error":{"code":"target_gone","message":"x"}}|} = Error { code = "target_gone"; message = "x" });
check_bool "items" true (Wire.items ~vtype:"i32" (`List [ `Assoc [ "addr", `String "0x10008"; "v", `String "100" ] ]) = [ { addr = 0x10008L; v = Vint 100L } ])
(* test_session.ml, each with a fresh fake helper *)
(* find sends the full plan in order *)
queue_scan f [ gi "i32" 241178; gi "f32" 18382 ] ; let o = ok (Session.find s "100") in
check_int "main" 259560 o.main; check_ids "scan groups" [ "i32"; "f32"; "i16"; "i64"; "f64"; "u8"; "i32x100"; "i32x10"; "i32x1000"; "f32pct"; "i32u"; "f32u" ] (last_scan_ids f); check_bool "keep" false (last_scan_keep f)
(* next number narrows atomically with reinterpreted predicates *)
queue_narrow f ~applied:true [ gi "i32" 2; gi "f32" 1 ]; let o = ok (Session.next_number s "85") in
check_int "main" 3 o.main; check_bool "atomic" true (last_narrow_atomic f); check_pred "i32x100" (Eq 8500L) (last_narrow_pred f "i32x100")
(* fallback to a relation when the exact number matches nothing *)
queue_narrow f ~applied:false []; queue_narrow f ~applied:true [ gi "i32" 4 ]; let o = ok (Session.next_number s "60") in
check_bool "fell back" true (List.mem (Session.Fell_back Down) o.notes); check_pred "relation" Lt_prev (last_narrow_pred f "i32")
(* dead end leaves the hunt as it was *)
queue_narrow f ~applied:false []; queue_narrow f ~applied:false []; let o = ok (Session.next_number s "12345") in
check_bool "dead end" true o.dead_end; check_int "unchanged" 4 o.main
(* untracked groups are rescanned with keep, and win over an empty narrow *)
(* after find where u8 came back tracked=false: *) queue_narrow f ~applied:false []; queue_scan f [ gi "u8" 3 ];
let o = ok (Session.next_number s "29") in check_bool "rescan keep" true (last_scan_keep f); check_int "wide" 3 o.wide;
check_bool "showing wide" true (List.mem Session.Showing_wide o.notes); check_bool "old groups dropped" true (dropped f <> [])
(* relation with no values *)
check_err "too many" "too many matches to compare by change yet. type the new number instead." (Session.next_relation s Down)
(* results, set, undo *)
queue_peek f "i32" [ it 0x10008L (Vint 85L) ]; let rows = ok (Session.results s ~all:false) in
check_str "row" "85" (List.hd rows).shown; check_str "tag" "whole number" (List.hd rows).tag;
queue_write f ~prev:(Vint 85L); check_str "set" "set #1 to 999 (was 85)" (ok (Session.set s ~row:1 "999"));
queue_write f ~prev:(Vint 999L); check_str "undo" "restored #1 to 85" (ok (Session.undo s));
check_err "undo twice" "nothing to undo" (Session.undo s);
check_err "bad row" "no result #9. run: moth results" (Session.set s ~row:9 "1");
check_err "does not fit" "999999 doesn't fit in a whole number (1-byte)" (Session.set s_u8 ~row:1 "999999"); check_int "nothing written" 0 (write_calls f_u8)
(* out of order, and the game closing *)
check_err "no hunt" "no hunt in progress. start one with: moth find <number>" (Session.next_number fresh "5");
queue_error f "narrow" { code = "target_gone"; message = "" };
check_err "closed" "the game closed. attach again once it's running." (Session.next_number s "5");
check_err "cleared" "no hunt in progress. start one with: moth find <number>" (Session.next_number s "5")
(* test_render.ml *)
check_str "counts" "259,560 matches (241,178 whole number, 18,382 decimal)"
  (Render.outcome { main = 259560; wide = 0; by_tag = [ "whole number", 241178; "decimal", 18382 ]; notes = []; dead_end = false; ms = 0. });
check_str "dead end" "nothing left that fits. that step was not applied, so you can try a different number."
  (Render.outcome { main = 4; wide = 0; by_tag = []; notes = []; dead_end = true; ms = 0. });
check_str "fallback" "4 matches (4 whole number)\nno exact match for that number. kept the ones that went down instead."
  (Render.outcome { main = 4; wide = 0; by_tag = [ "whole number", 4 ]; notes = [ Fell_back Down ]; dead_end = false; ms = 0. })
```

- [ ] **Step 2: Run `opam exec -- dune test`** — expect compile failure.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run `opam exec -- dune test`** — expect pass.
- [ ] **Step 5: Commit** — `git commit -m "core: the hunt, wire codec and user-facing text"`

---

### Task 10: Reaching the helper in a bottle

**Files:**
- Create: `core/lib/crossover.ml`, `core/lib/crossover.mli`, `core/lib/helper_conn.ml`, `core/lib/helper_conn.mli`, `core/lib/anticheat.ml`, `core/lib/anticheat.mli`
- Test: `core/test/test_crossover.ml`, `core/test/test_helper_conn.ml`, `core/test/test_anticheat.ml`

**Interfaces:**
- Consumes: `Wire`, `Helper_api`.
- Produces:
```ocaml
(* crossover.mli *)
val find_app : unit -> (string, string) result          (* MOTH_CROSSOVER, ~/Applications/CrossOver.app, /Applications/CrossOver.app *)
val bottles_dir : unit -> string                         (* MOTH_BOTTLES or ~/Library/Application Support/CrossOver/Bottles *)
val list_bottles : unit -> string list                   (* sorted names of directories containing cxbottle.conf *)
val check_bottle : string -> (unit, string) result
val wine_argv : app:string -> bottle:string -> exe:string -> string list -> string array
(* helper_conn.mli *)
type launcher = port:int -> token:string -> log:string -> (unit, string) result
val wine_launcher : bottle:string -> helper_exe:string -> launcher
val start : launch:launcher -> label:string -> log:string -> ?timeout:float -> unit -> (Helper_api.helper, string) result
val helper_exe : unit -> (string, string) result         (* MOTH_HELPER, else moth-helper.exe beside the moth executable *)
(* anticheat.mli *)
val check : processes:string list -> modules:string list -> string option
```
- `start`: listen on `127.0.0.1` port 0; make a 32-hex-character token from `/dev/urandom`; call `launch`; accept connections until one sends a hello with the right token (a wrong token closes that connection and keeps waiting); default timeout 60 seconds. Per-request timeouts: 300 seconds for `scan` and `narrow`, 30 seconds otherwise. A closed connection gives `err { code = "helper_lost"; ... }`.
- `wine_launcher` starts wine with stdout and stderr appended to `log` and does not wait for it.
- Error copy:
  - `crossover not found. set MOTH_CROSSOVER to the path of CrossOver.app.`
  - `no bottle called "<name>". bottles: <a>, <b>, ...`
  - `the helper didn't start in <label> (waited <n>s). see <log>`
- `Anticheat.check` is case-insensitive on file names and returns a display name:
  - "easy anti-cheat": `easyanticheat.exe`, `easyanticheat_eos.exe`, `easyanticheat_x64.dll`, `easyanticheat_x86.dll`, `eac_launcher.exe`
  - "battleye": `beservice.exe`, `beservice_x64.exe`, `beclient.dll`, `beclient_x64.dll`
  - "vanguard": `vgc.exe`, `vgtray.exe`
  - "ea anticheat": `eaanticheat.gameservice.exe`, `eaanticheat.gameservicelauncher.exe`
  - "punkbuster": `pnkbstra.exe`, `pnkbstrb.exe`
  - "gameguard": `gamemon.des`, `gamemon64.des`
  - "xigncode": `xxd-0.xem`, `x3.xem`
  - "faceit": `faceit.exe`, `faceitclient.exe`
  - "ace": `sguard64.exe`, `sguardsvc64.exe`, `ace-base.dll`

- [ ] **Step 1: Write the failing tests**

```ocaml
(* test_crossover.ml: temp dirs, env set per test *)
check_err "missing app" "crossover not found. set MOTH_CROSSOVER to the path of CrossOver.app." (with_env [ "MOTH_CROSSOVER", "/nonexistent.app"; "HOME", tmp ] Crossover.find_app);
(* tmp/b/Disco Elysium/cxbottle.conf, tmp/b/uhmm/cxbottle.conf, tmp/b/notabottle/ *)
check_list "bottles" [ "Disco Elysium"; "uhmm" ] (Crossover.list_bottles ());
check_err "unknown bottle" {|no bottle called "nope". bottles: Disco Elysium, uhmm|} (Crossover.check_bottle "nope");
check_argv "argv" [| app ^ "/Contents/SharedSupport/CrossOver/bin/wine"; "--bottle"; "Disco Elysium"; "--workdir"; "/x/bin"; "/x/bin/moth-helper.exe"; "5000"; "tok" |]
  (Crossover.wine_argv ~app ~bottle:"Disco Elysium" ~exe:"/x/bin/moth-helper.exe" [ "5000"; "tok" ])
(* test_helper_conn.ml: the launcher starts a thread that plays the helper over TCP *)
let h = ok (Helper_conn.start ~launch:(fake_helper_thread ~token:`Right) ~label:"test" ~log ()) in
check_bool "ps" true (h.ps () = Ok [ 208, "disco.exe" ]);
let h = ok (Helper_conn.start ~launch:(fake_helper_thread ~token:`Wrong_then_right) ~label:"test" ~log ()) in check_bool "accepted second" true (Result.is_ok (h.ps ()));
check_err "never connects" ("the helper didn't start in test (waited 1s). see " ^ log) (Helper_conn.start ~launch:(fun ~port:_ ~token:_ ~log:_ -> Ok ()) ~label:"test" ~log ~timeout:1.0 ());
(* fake helper closes the socket mid-session *) check_bool "lost" true (match h.ps () with Error { code = "helper_lost"; _ } -> true | _ -> false);
(* fake helper answers a scan with an error *) check_bool "error mapped" true (match h.scan ~keep:false [] with Error { code = "not_attached"; _ } -> true | _ -> false)
(* test_anticheat.ml *)
check_opt "eac" (Some "easy anti-cheat") (Anticheat.check ~processes:[ "game.exe"; "EasyAntiCheat_EOS.exe" ] ~modules:[]);
check_opt "battleye module" (Some "battleye") (Anticheat.check ~processes:[] ~modules:[ "BEClient_x64.dll" ]);
check_opt "clean" None (Anticheat.check ~processes:[ "disco.exe"; "explorer.exe" ] ~modules:[ "UnityPlayer.dll"; "GameAssembly.dll" ])
```

- [ ] **Step 2: Run `opam exec -- dune test`** — expect compile failure.
- [ ] **Step 3: Implement.** Use `Unix.select` for the accept and read timeouts. The test's fake helper needs the `threads.posix` library in the test stanza only.
- [ ] **Step 4: Run `opam exec -- dune test`** — expect pass.
- [ ] **Step 5: Commit** — `git commit -m "core: find crossover, launch and talk to the helper, anti-cheat refuse list"`

---

### Task 11: The daemon and the `moth` command

**Files:**
- Create: `core/lib/daemon.ml`, `core/lib/daemon.mli`, `core/lib/client.ml`, `core/lib/client.mli`
- Modify: `core/bin/main.ml`, `Makefile` (copy `moth` to `bin/`)
- Test: `core/test/test_daemon.ml`

**Interfaces:**
- Consumes: `Session`, `Render`, `Helper_conn`, `Crossover`, `Anticheat`.
- Produces:
```ocaml
(* daemon.mli *)
type start_helper = bottle:string -> (Helper_api.helper, string) result
val run : home:string -> start_helper:start_helper -> unit      (* serves until "stop" *)
val default_start_helper : home:string -> start_helper
(* client.mli *)
val home : unit -> string                                        (* MOTH_HOME or ~/Library/Application Support/moth *)
val call : ?autostart:bool -> home:string -> actor:string -> cmd:string -> args:string list -> opts:(string * string) list -> unit -> (Yojson.Safe.t * string, string) result
```
- Commands (`cmd`, args, opts):
  - `bottles` → names. `ps` with opt `bottle` → Windows processes in that bottle.
  - `attach` `<name-or-pid>` with opt `bottle`. Name match is exact and case-insensitive. Flow: start helper, `ps`, pick the process, `modules`, `Anticheat.check` on the process names and modules. A hit detaches and returns `moth won't attach: <name> is running with this game. moth is for single-player, offline play.` Several processes with the same name: `more than one <name> is running (pids <a>, <b>). attach by pid.` None: `<name> isn't running in <bottle>. start the game first.`
  - `status`, `find <number>`, `next <number|down|up|same|changed>`, `results` (opt `all`), `set <#n> <number>` (the `#` is optional), `undo`, `detach`, `log` (opt `n`, default 20), `stop`.
  - Anything needing a target before `attach`: `not attached. run: moth attach --bottle <bottle> <game.exe>`.
- One target at a time. `ps` and `attach` start a helper in the named bottle and reuse it. `detach`, `stop`, and attaching in a different bottle close the helper's connection, which makes the helper exit.
- The daemon handles one request at a time, in arrival order. `home` is created with mode `0700`, the socket with `0600`.
- Every handled request appends one line to `$MOTH_HOME/events.jsonl`: `{"seq","ts","actor","cmd","args","ok","summary"}`, where `summary` is the rendered text's first line. `log` returns the last `n`.
- Client auto-start: if connecting fails, remove a stale `core.sock`, start `moth daemon` detached with output appended to `$MOTH_HOME/core.log`, and retry for up to 5 seconds. Then: `couldn't start moth's background service. see <home>/core.log`.
- `moth <cmd> [args] [--bottle <b>] [--all] [--json] [-n <n>]`. Prints the reply's `text` (or the raw `result` with `--json`). Exit 0 on success, 1 on an error reply (message on stderr), 2 on bad usage. `moth` with no arguments or `moth help` prints a short usage listing these commands. `moth daemon` runs the daemon in the foreground.

- [ ] **Step 1: Write the failing tests** (each starts `Daemon.run` on a thread with a temp `home` and a fake `start_helper`)

```ocaml
check_err "before attach" "not attached. run: moth attach --bottle <bottle> <game.exe>" (call "find" [ "100" ]);
check_str "status" "not attached" (text (call "status" []));
(* fake helper: ps = [208, "disco.exe"; 300, "explorer.exe"], modules 208 = ["UnityPlayer.dll"] *)
check_str "attach" "attached to disco.exe (pid 208, 64-bit) in uhmm" (text (call "attach" [ "DISCO.EXE" ] ~opts:[ "bottle", "uhmm" ]));
check_err "not running" "nope.exe isn't running in uhmm. start the game first." (call "attach" [ "nope.exe" ] ~opts:[ "bottle", "uhmm" ]);
(* fake helper with EasyAntiCheat_EOS.exe in ps *)
check_err "anticheat" "moth won't attach: easy anti-cheat is running with this game. moth is for single-player, offline play." (call "attach" [ "game.exe" ] ~opts:[ "bottle", "uhmm" ]);
(* fake scan reply i32 = 3 *) check_str "find" "3 matches (3 whole number)" (text (call "find" [ "100" ]));
check_bool "json" true (Yojson.Safe.Util.(member "main" (json (call "find" [ "100" ])) |> to_int) = 3);
check_err "usage" "next needs a number, or one of: down, up, same, changed" (call "next" []);
check_bool "set accepts #" true (Result.is_ok (call "set" [ "#1"; "999" ]));
(* events *) let ev = json (call "log" [] ~opts:[ "n", "2" ]) in check_str "actor" "agent" Yojson.Safe.Util.(index 0 ev |> member "actor" |> to_string);   (* previous call sent ~actor:"agent" *)
(* stale socket: create a plain file at home/core.sock, no daemon *) check_bool "autostart cleans stale socket" true (Result.is_ok (Client.call ~home ~actor:"user" ~cmd:"status" ~args:[] ~opts:[] ()));
(* second daemon *) check_bool "second daemon refuses" true (second_daemon_exits_with "moth is already running");
check_str "stop" "stopped" (text (call "stop" [])); check_bool "socket gone" false (Sys.file_exists (home ^ "/core.sock"))
```
The auto-start test needs the real `moth` executable; give the client an injectable `spawn_daemon` for tests, defaulting to re-running `Sys.executable_name` with `daemon`.

- [ ] **Step 2: Run `opam exec -- dune test`** — expect compile failure.
- [ ] **Step 3: Implement.** Hand-rolled argument parsing, no extra dependency.
- [ ] **Step 4: Run `opam exec -- dune test`, then `make build && bin/moth bottles`** — tests pass, and the real bottles are listed (`Disco Elysium`, `uhmm`, ...).
- [ ] **Step 5: Commit** — `git commit -m "core: daemon, client and the moth command"`

---

### Task 12: End to end in a bottle

**Files:**
- Create: `core/integration/dune`, `core/integration/it_main.ml`, `core/integration/target_ctl.ml`
- Modify: `Makefile` (`test-integration`, `bench`)

**Interfaces:**
- Consumes: `bin/moth`, `bin/moth-helper.exe`, `bin/moth-target.exe`, `bin/moth-target32.exe`, the `WINE` and `MOTH_TEST_BOTTLE` variables.
- Produces: `make test-integration` (runs every scenario, exits non-zero on any failure, prints one line per scenario) and `make bench` (scenario 10 alone). Both use a temp `MOTH_HOME` and stop the daemon and target afterwards, even on failure.
- `Target_ctl.start : exe:string -> filler_mb:int -> t` launches the target in the test bottle and accepts its connection; `vars`, `set`, `get`, `quit`. Scenarios drive `bin/moth ... --json` as a subprocess.

- [ ] **Step 1: Write the scenarios** (each asserts on `--json` output and on what the target reports)

1. **Decimal found from a whole number.** `find 100`; target `set health 85.3`; `next 85`; target `set health 61.9`; `next 61`. The results contain `health`'s address with tag `decimal`. `set <that row> 999`; target `get health` is `999`.
2. **Money typed with decimals.** `find 132.50`; `set money_cents 9925`; `next 99.25`. Results contain `money_cents` with tag `whole number ×100`. `set <row> 999.99`; target reports `99999`.
3. **One-byte value.** `find 30`; `set ammo 29`; `next 29`; `set ammo 12`; `next 12`. Results (with `--all` if needed) contain `ammo` with tag `whole number (1-byte)`.
4. **Unaligned value surfaces on its own.** `find 4242`; `set gems 4300`; `next 4300`. The reply's notes include showing-wide, and results contain `gems` with tag `whole number (unaligned)` without `--all`.
5. **Fallback to a relation.** `find 100`; `set health 80`, `set health_copy 80`; `next 77`. The reply says it fell back to went down, and results still contain `health_copy`.
6. **Dead end changes nothing.** `find 100`; `next 12345`. The reply is a dead end, and `status` shows the same counts as after `find`.
7. **Undo.** After scenario 1's `set`, `undo`; target `get health` is back to `61.9`.
8. **32-bit target.** With `moth-target32.exe`: `attach` reports 32-bit; `find 100`, `set health_copy 64`, `next 64` finds `health_copy`.
9. **A tiny, common number.** Target started with `--filler-small-mb 256`. `find 3` reports at least 8,000,000 main matches, and `status` shows no main-tier group untracked. `set lives 2`; `next 2`; `set lives 1`; `next 1`. Results contain `lives` with tag `whole number`.
10. **Speed.** Target started with `--filler-mb 1024`. `find 100` reply's scan time divided by gigabytes scanned is at most `3.0`. Print `scan: <x> s/GB`, then time one `next 100` and print it.
11. **The game closing.** `find 100`; target `quit`; `next 85` fails with `the game closed. attach again once it's running.`
12. **Anti-cheat names.** Not run in a bottle (covered by Task 11's fake). Listed here so the gap is visible.

- [ ] **Step 2: Run `make test-integration`** — expect failures to be real defects in Tasks 2 to 11, not in the scenarios. Fix each in the module that owns it, with a unit test added there first.
- [ ] **Step 3: Run `make test-integration` until all scenarios print `ok`, then `make bench`.** Record the printed figures in `docs/dev.md`.
- [ ] **Step 4: Run `make test`** — unit tests still pass.
- [ ] **Step 5: Commit** — `git commit -m "integration: bottle scenarios against the stand-in target"`

---

### Task 13: Prove it on Disco Elysium

This task needs Chris at the keyboard. Do not mark stage 1 done without it.

**Files:**
- Create: `docs/verification/disco-elysium.md`

- [ ] **Step 1: Chris starts Disco Elysium from CrossOver and loads a save.** He reads out three numbers on screen: money, and two others of his choice (for example health, morale, a skill value or experience).
- [ ] **Step 2: Attach.** Run `bin/moth attach --bottle "Disco Elysium" disco.exe`. Expected: `attached to disco.exe (pid <n>, 64-bit) in Disco Elysium`.
- [ ] **Step 3: For each of the three values:** `bin/moth find <number as shown>`, Chris changes the value in the game, `bin/moth next <new number>`, repeat until `bin/moth results` shows a handful, then `bin/moth set <row> <new number>` and Chris confirms the change on screen. Only numbers are typed, never a type.
- [ ] **Step 4: Record in `docs/verification/disco-elysium.md`:** date, game build, memory scanned (GB), first-scan time and seconds per gigabyte, each narrowing time and survivor count, the tag moth gave each found value, whether the change showed in game, and anything that failed. For each value, note which "value type" Cheat Engine would have needed to find it, as the head-to-head line.
- [ ] **Step 5: Check the bar.** First scan at 3 seconds per gigabyte or better, three values found and changed. If a value cannot be found, stop and investigate with the stand-in target before changing anything, and add the missing case to Task 12's scenarios.
- [ ] **Step 6: Commit** — `git commit -m "verification: disco elysium, stage 1"`
