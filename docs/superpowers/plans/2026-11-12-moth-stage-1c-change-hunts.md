# moth stage 1c (hunting with no number) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Find a value that has no number on screen by saying only how it moved (down, up, same, changed), starting from a snapshot of the game's memory, and close the two bars stage 1b missed on Disco Elysium.

**Architecture:** No new processes. The helper gains a snapshot: a copy of the game's writable memory that stands in for "the previous value" of every place until a group of candidates is small enough to keep its own. Narrowing runs on the same worker threads as scanning, and stored values take only as many bytes as their type, so far larger groups can remember what they held. The core adds a snapshot hunt beside the number hunt, and kept values learn to say when the game put a written value back.

**Tech Stack:** As stages 1 and 1b: OCaml 5.5.x, dune, yojson, alcotest; C++20, CMake, mingw-w64, doctest, nlohmann/json.

**Spec:** `docs/superpowers/specs/2026-11-11-moth-design.md` (section 6.4 "No number to type" and "Honest dead end", section 8 on where a snapshot lives, risk 6). The misses this plan answers are in `docs/verification/disco-elysium.md`, the stage 1b section's "result" and "problems found". The helper protocol table is in `docs/superpowers/plans/2026-11-11-moth-stage-1-engine.md` ("Protocols") with additions in `docs/superpowers/plans/2026-11-11-moth-stage-1b-disco-gaps.md` ("Protocol changes"); this plan's "Protocol changes" adds to both.

## Global Constraints

- Everything in the stage 1 plan's Global Constraints still holds: OCaml and C++ only; SIP on, no `sudo`, CrossOver untouched; memory read and written from outside only; the user never supplies a data type on any command; lowercase plain user-facing text, no emojis; local git only, never a remote; lowercase commit messages with no attribution trailers; no edits to global configuration.
- One hunt at a time (spec 6.3). A snapshot hunt replaces a number hunt and the other way round. Kept values outlast both.
- Every protocol addition is a new op or an optional field. An existing request behaves exactly as before.
- A snapshot lives in the helper's memory. Its size is limited (8 GiB unless configured), checked before anything is read, and refused in plain words when over.
- The first-scan bar stays 3.0 s/GiB. A snapshot, and each narrowing step from a snapshot, has the same bar.
- Automated tests use only the `uhmm` bottle and a temporary `MOTH_HOME`, and leave no `moth daemon`, `moth-helper` or `moth-target` process running. The shared test fake (`core/test/fake_helper.ml`) stays strict.
- A task that changes a rule updates the existing tests that pin the old rule, keeping what each test proves, and says which in its report. Every other existing test stays unchanged.
- All paths are relative to `~/Documents/moth`.

## Review Focus

1. **The game allocates or frees memory between the snapshot and a step.** Memory that appeared later is not a candidate (it has no previous value). Memory that went away is counted as lost. A candidate is never compared with bytes from a different address. Pinned in Task 3.
2. **A snapshot that is too big, or that the helper cannot get memory for.** Refused in plain words. When the size check refuses, the hunt in progress is untouched. Pinned in Tasks 3 and 6.
3. **"Same" as the very first step, and many in a row.** Billions of places survive. Memory stays within the snapshot plus the candidate budget, each step meets the time bar, and the counts are right. Pinned in Tasks 3 and 8.
4. **A number, or a write, in the middle of a snapshot hunt.** `next 97` after change steps works. After a `set`, the next relation compares with the written value. Pinned in Tasks 3 and 6.
5. **A second snapshot, or a `find`, while a snapshot is held.** The old snapshot is released before the new work reads anything, so memory never doubles. Kept values survive. Pinned in Tasks 3 and 6.

## Not in this plan

- Games whose writable memory is over the snapshot limit. Compressing the snapshot or keeping it on disk is later work; until then moth says the game is too big for a snapshot.
- Listing matches near a kept value beyond 4 kb, structure maps, restart-proof paths. Stage 3.
- The app. Stage 2.

## Protocol changes

**Helper protocol**

- New op `snapshot`: `{"op":"snapshot","groups":[{"id":"i32","type":"i32","align":4}],"cap_mb":8192,"budget_mb":1536}` (`cap_mb` and `budget_mb` optional). It replaces every group. Each listed group becomes every aligned place in the game's writable memory, with the snapshot as its previous values. Reply: `groups:[I], stats:S`, as `scan` gives. In `I`, `has_values` is true for a group while the snapshot or its own stored values can answer a previous-value comparison.
- New error code `too_big`: the writable memory is over the cap, or the helper could not get the memory. The message carries both sizes in bytes when the cap refused.
- `narrow` is unchanged on the wire. Previous-value predicates and `moved` work on a snapshot-backed group.

**Client protocol**

- New command `snapshot` (no arguments). Result: the same shape as `find`, with `"snapshot"` among its `notes`.
- `kept` items gain `"set_to": "<shown>"|null` and `"put_back": bool`.

## File Structure

```
helper/engine/engine.hpp .cpp      width-sized values, bigger value cap, narrow on workers, snapshot hunts
helper/engine/snapshot.hpp .cpp    Snapshot: the copied regions and their bytes (new)
helper/engine/posset.hpp .cpp      add_full_block
helper/proto/handler.cpp           op snapshot, error too_big
core/lib/helper_api.ml             snapshot in the helper record
core/lib/wire.ml, helper_conn.ml   the new op
core/lib/interp.ml .mli            blind_plan
core/lib/session.ml .mli           snapshot hunts; kept values that were put back; keep reads now
core/lib/render.ml .mli            snapshot line, dead-end and too-many texts, kept suffix
core/lib/daemon.ml, core/bin/main.ml   snapshot command, MOTH_SNAPSHOT_MB, help
tests/target/target.cpp            a value with no number; a 1 GiB snapshot target
core/integration/it_main.ml        scenarios 25 to 30
docs/dev.md                        MOTH_SNAPSHOT_MB
docs/verification/disco-elysium.md stage 1c section (Task 9)
```

---

### Task 1: Stored values take their type's width, and far larger groups keep them

**Files:**
- Modify: `helper/engine/engine.hpp`, `helper/engine/engine.cpp`
- Test: `helper/test/test_engine_narrow.cpp`, `helper/test/test_engine_scan.cpp`

**Interfaces:**
- Changes: a group's last-seen values are stored in as many bytes each as its type is wide (they were 8 bytes each). `EngineConfig::value_cap` defaults to `64'000'000` (it was `2'000'000`). The existing budget rules are unchanged: values a group would gain are not kept when they would take storage over `budget_bytes`.
- Produces: nothing new on any interface. `GroupInfo::has_values` is simply true for far larger groups.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("a group of two million floats keeps its values, four bytes each") {
  const uint64_t base = 0x100000, size = 8u << 20;
  FakeMemory m; m.add_region(base, size);
  for (uint64_t a = base; a < base + size; a += 4) m.poke<float>(a, 0.5f);   // 2,097,152 floats
  m.poke<float>(base, 1.0f);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  auto info = e.scan({G("f", F32, 4, 0, range_floor(0, 2, 1e-6))}, false, s);
  CHECK(info[0].count == 2097152); CHECK(info[0].has_values);      // over the old cap of 2,000,000
  CHECK(e.storage_bytes() < (12u << 20));                           // four bytes a value, not eight
  m.poke<float>(base, 0.25f);
  Pred p = range_floor(-1, 1, 1e-6); p.moved = true;
  CHECK(e.narrow({{"f", p}}, true, g, s)); CHECK(g[0].count == 1); CHECK(e.peek("f", 0, 9)[0].addr == base);
}
```

Also one test each: stored values of every width (1, 2, 4, 8 bytes) survive a scan, a relation narrow and a `write_value` with the right value (reuse the existing relation tests' shape for `U8`, `I16`, `F32`, `F64`); a group over `value_cap` still has no values and answers `no_values` (set `.value_cap = 2`, as existing tests do).

- [ ] **Step 2: Run it and see it fail**

Run: `make test-native`
Expected: FAIL on `has_values` (the group is over the old cap).

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run the tests and the bench**

Run: `make test-native && make bench-native` (note the bench figures before starting and after)
Expected: all pass. The scan figure may rise for a search whose groups now keep values; report both figures. It must stay under 3.0 s/GiB.

- [ ] **Step 5: Commit**

```bash
git add helper && git commit -m "helper: stored values take their type's width, and groups up to 64 million keep them"
```

### Task 2: Narrowing runs on the scan's workers

**Files:**
- Modify: `helper/engine/engine.hpp`, `helper/engine/engine.cpp`
- Test: `helper/test/test_engine_parallel.cpp`, `helper/test/bench.cpp`

**Interfaces:**
- Changes: `Engine::narrow` reads its runs on the calling thread in address order and compares them on `workers_used()` threads, as `scan` does. The groups it leaves, their order, their stored values and `stats.lost` are identical for any worker count. A worker never holds more than one run's bytes.
- Produces: the bench prints a narrow figure for a dense group (every aligned 4-byte place of the bench's memory, an `EqPrev` step) in seconds per GiB, on one worker and on the automatic count.

- [ ] **Step 1: Write the failing test**

In `test_engine_parallel.cpp`, in the style of the existing "same result for any worker count" scan test: build 64 MiB of memory with a mixed pattern, scan three groups (`I32` eq, `F32` range, `U8` eq), change a spread of values, and narrow with `workers` 1, 2, 4 and 8. For each count, `peek` every group in full and compare addresses and values with the one-worker result, and compare `stats.lost` after freeing one region between scan and narrow. Add an assertion that fails today: a counter of the most threads that compared at once during the last narrow (`unsigned narrow_threads_used() const`) is above 1 when `workers` is 4.

- [ ] **Step 2: Run it and see it fail**

Run: `make test-native`
Expected: compile error on `narrow_threads_used`.

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run the tests and the bench**

Run: `make test-native && make bench-native`
Expected: all pass; the dense narrow figure on the automatic worker count is at most half the one-worker figure. Report both.

- [ ] **Step 5: Commit**

```bash
git add helper && git commit -m "helper: a narrowing step compares on the scan's workers"
```

### Task 3: The snapshot

**Files:**
- Create: `helper/engine/snapshot.hpp`, `helper/engine/snapshot.cpp`
- Modify: `helper/engine/engine.hpp`, `helper/engine/engine.cpp`, `helper/engine/posset.hpp`, `helper/engine/posset.cpp`, `helper/CMakeLists.txt`
- Test: `helper/test/test_engine_snapshot.cpp` (new), `helper/test/bench.cpp`

**Interfaces:**
- Produces:

```cpp
// posset.hpp: every aligned offset of a block, stored as a bitmap
void PositionSet::add_full_block(uint64_t base, uint32_t span);

// snapshot.hpp: the bytes of the regions that were readable when it was taken
class Snapshot {
 public:
  uint64_t bytes() const;                                   // bytes held
  bool empty() const;
  const uint8_t* at(uint64_t addr, size_t len) const;       // the bytes saved for [addr, addr+len), or null if not all held
  // adding and replacing bytes is the engine's business; keep those members private or friend-only
};

// engine.hpp
struct SnapSpec { std::string id; ValueType type; uint32_t align; };
struct EngineConfig { /* ... */ uint64_t snapshot_cap_bytes = 8ull << 30; };
std::vector<GroupInfo> Engine::snapshot(const std::vector<SnapSpec>& specs, ScanStats& stats);
uint64_t Engine::snapshot_bytes() const;
void Engine::set_snapshot_cap_bytes(uint64_t b);
```

- Rules:
  - `snapshot` first adds up the writable regions. Over `snapshot_cap_bytes`: `EngineError("too_big", "<bytes> to snapshot, limit <bytes>")`, and nothing changes (groups and any earlier snapshot stay).
  - Otherwise it releases every group and any earlier snapshot, then reads every writable region in blocks as `scan` does and keeps the bytes that arrived. Each listed group gets every aligned place of the bytes held (`add_full_block`), `tracked` and `has_values` true. `stats` is filled as for a scan. Groups over `budget_bytes` are untracked from the last one backwards, as after a scan. If memory cannot be had (`std::bad_alloc`), the snapshot and groups are released and it throws `too_big` with the message `could not get memory for the snapshot`.
  - `scan` with `keep = false` releases the snapshot. `scan` with `keep = true` leaves it. `drop` of every snapshot-backed group releases it.
  - In `narrow`, a group with no stored values of its own but covered by the snapshot takes each place's previous value from the snapshot. After an applied step, a group at or under `value_cap` stores the values just read (as today) and no longer needs the snapshot; for the places of groups that still need it, the snapshot's bytes are replaced with the bytes just read, so the next relation compares with this step. When no group needs the snapshot, it is released. A step that is not applied changes nothing, the snapshot included.
  - A place whose memory can no longer be read is lost. Memory that became readable after the snapshot is never a candidate.
  - `write_value` also replaces the snapshot's bytes at that address when the snapshot holds it.
  - `storage_bytes()` does not include the snapshot; `snapshot_bytes()` reports it.

- [ ] **Step 1: Write the failing tests** (`test_engine_snapshot.cpp`)

```cpp
TEST_CASE("a snapshot hunt narrows by change alone") {
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<int32_t>(0x10010, 100); m.poke<int32_t>(0x10020, 7);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  auto info = e.snapshot({{"i32", I32, 4}}, s);
  CHECK(info[0].count == 1024); CHECK(info[0].tracked); CHECK(info[0].has_values); CHECK(e.snapshot_bytes() == 4096);
  m.poke<int32_t>(0x10010, 90); m.poke<int32_t>(0x10020, 3);
  CHECK(e.narrow({{"i32", {Pred::LtPrev}}}, true, g, s)); CHECK(g[0].count == 2);
  CHECK(e.snapshot_bytes() == 0);                       // two places keep their own values now
  m.poke<int32_t>(0x10010, 95);                         // up from 90, though still under the 100 of the snapshot
  CHECK(e.narrow({{"i32", {Pred::GtPrev}}}, true, g, s)); CHECK(g[0].count == 1); CHECK(e.peek("i32", 0, 9)[0].addr == 0x10010);
}
TEST_CASE("same first: the snapshot moves on with each step") {
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<int32_t>(0x10010, 100);
  Engine e(m, {.value_cap = 4}); ScanStats s; std::vector<GroupInfo> g;
  e.snapshot({{"i32", I32, 4}}, s);
  m.poke<int32_t>(0x10020, 5);                                              // something else changes
  CHECK(e.narrow({{"i32", {Pred::EqPrev}}}, true, g, s)); CHECK(g[0].count == 1023); CHECK(g[0].has_values);
  CHECK(e.snapshot_bytes() == 4096);                                        // 1023 places still lean on it
  m.poke<int32_t>(0x10010, 90);
  CHECK(e.narrow({{"i32", {Pred::LtPrev}}}, true, g, s)); CHECK(g[0].count == 1); CHECK(e.snapshot_bytes() == 0);
}
TEST_CASE("a snapshot over the limit is refused and the hunt stays") {
  FakeMemory m; three(m); Engine e(m, {.snapshot_cap_bytes = 1024}); ScanStats s;
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  CHECK_THROWS_WITH_AS(e.snapshot({{"i32", I32, 4}}, s), doctest::Contains("too_big"), EngineError);
  CHECK(e.groups().size() == 1); CHECK(e.groups()[0].count == 3);
}
```

Also one test each:
- two groups of different types from one snapshot (`I32` and `F32`) each get their own answer to `LtPrev` for the same change;
- a region freed after the snapshot: its places are lost at the next step, counted in `stats.lost`; a region added after the snapshot adds no candidates;
- a region that could not be read at snapshot time is left out and counted in `unreadable_bytes`;
- a number step on a fresh snapshot (`eq(100)`) works and leaves the one place;
- an atomic step that would leave nothing changes nothing: counts and `snapshot_bytes()` as before, and the next relation still compares with the snapshot;
- `write_value` to a snapshot-backed place, then the game lowers it: `LtPrev` keeps it (the comparison is with the written value);
- a second `snapshot` while one is held never holds both (assert through `snapshot_bytes()` right after, and a `FakeMemory` hook or counter if needed), and `scan` with `keep = false` releases the snapshot;
- groups over the budget are untracked from the last backwards (`budget_bytes` small), and a step on the tracked ones still works;
- the result of a step is the same on 1 and 4 workers.

In `bench.cpp`: print the snapshot's time and a first `EqPrev` step's time in seconds per GiB, for the plan's six groups (see Task 5), on the automatic worker count.

- [ ] **Step 2: Run them and see them fail**

Run: `make test-native`
Expected: compile errors for `Engine::snapshot`.

- [ ] **Step 3: Implement**

`Snapshot` owns the bytes region by region. The engine reads a step's memory block by block: compare a block against the snapshot's bytes for every group that leans on it, then replace the snapshot's bytes for that block, so no more than a block's worth of extra bytes per worker is ever held.

- [ ] **Step 4: Run the tests and the bench**

Run: `make test-native && make bench-native && make helper`
Expected: all pass, no warnings in either build; the bench's snapshot and step figures are both under 3.0 s/GiB natively. Report them.

- [ ] **Step 5: Commit**

```bash
git add helper && git commit -m "helper: a snapshot of the game's memory stands in for previous values"
```

### Task 4: The snapshot on the wire

**Files:**
- Modify: `helper/proto/handler.cpp`, `helper/test/fake_platform.hpp` (only if needed)
- Test: `helper/test/test_handler.cpp`

**Interfaces:**
- Consumes: `Engine::snapshot`, `set_snapshot_cap_bytes`.
- Produces: the op in "Protocol changes". `groups` items need `id`, `type`, `align` (positive); a `pred` field, an unknown type or an id listed twice is `bad_request`. `cap_mb` (a whole number from 1 to 1048576) sets the cap for this and later snapshots; `budget_mb` follows the same rule as on `scan` with `keep:false`. The reply's group types are remembered for `peek`, as after a `scan`. `too_big` passes through with the engine's message.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("snapshot, then narrow by change, over the wire") {
  FakePlatform p; p.add(208, "disco.exe", 64); p.mem(208).add_region(0x10000, 4096); p.mem(208).poke<int32_t>(0x10010, 100);
  Handler h(p); h.handle_line(R"({"id":1,"op":"attach","pid":208})");
  auto sn = J(h.handle_line(R"({"id":2,"op":"snapshot","groups":[{"id":"i32","type":"i32","align":4},{"id":"f32","type":"f32","align":4}]})"));
  CHECK(sn["ok"] == true); CHECK(sn["groups"][0]["count"] == 1024); CHECK(sn["groups"][0]["has_values"] == true); CHECK(sn["stats"]["bytes"] == 4096);
  p.mem(208).poke<int32_t>(0x10010, 90);
  auto nr = J(h.handle_line(R"({"id":3,"op":"narrow","atomic":true,"groups":[{"id":"i32","pred":{"op":"lt_prev"}},{"id":"f32","pred":{"op":"lt_prev"}}]})"));
  CHECK(nr["applied"] == true); CHECK(nr["groups"][0]["count"] == 1);
  auto pk = J(h.handle_line(R"({"id":4,"op":"peek","group":"i32","offset":0,"limit":5})"));
  CHECK(pk["items"][0]["addr"] == "0x10010"); CHECK(pk["items"][0]["v"] == "90");
}
```

Also: `"cap_mb":0`, a group with a `pred`, and an id listed twice are `bad_request`; a region larger than `"cap_mb":1` answers `too_big` and a `status` afterwards still lists the groups there were; `snapshot` before `attach` is `not_attached`.

- [ ] **Step 2: Run it and see it fail**

Run: `make test-native`
Expected: FAIL with `unknown_op`.

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run the tests**

Run: `make test-native && make helper`
Expected: all pass, no warnings.

- [ ] **Step 5: Commit**

```bash
git add helper && git commit -m "helper: snapshot on the wire"
```

### Task 5: The core's types for a snapshot

**Files:**
- Modify: `core/lib/helper_api.ml`, `core/lib/wire.ml`, `core/lib/wire.mli`, `core/lib/helper_conn.ml`, `core/lib/interp.ml`, `core/lib/interp.mli`, `core/lib/daemon.ml` (only to compile), `core/test/fake_helper.ml`
- Test: `core/test/test_interp.ml`, `core/test/test_helper_conn.ml`

**Interfaces:**
- Produces, in `Helper_api.helper`: `snapshot : cap_mb:int option -> budget_mb:int option -> (string * string * int) list -> (group_info list * stats, err) result` (each group is id, type, align).
- Produces, in `Interp`: `val blind_plan : t list`, the interpretations a snapshot hunt carries, in priority order: main `i32` (align 4), `f32` (4), `i16` (2), `i64` (8), `f64` (8); wide `u8` (1). All `Plain`, `exclude_align` 0, with the same ids and tags those interpretations have in `plan`.
- Produces, in `Fake_helper`: `Snapshot of { cap_mb; budget_mb; groups }` and `queue_snapshot`, in the style of `queue_scan`.

- [ ] **Step 1: Write the failing tests**

```ocaml
let blind_plan () =
  let ids = List.map (fun (i : Interp.t) -> (i.id, i.vtype, i.align, i.tier = Interp.Main)) Interp.blind_plan in
  check_bool "plan" true
    (ids = [ ("i32", "i32", 4, true); ("f32", "f32", 4, true); ("i16", "i16", 2, true); ("i64", "i64", 8, true); ("f64", "f64", 8, true); ("u8", "u8", 1, false) ]);
  check_str "tags as in a number hunt" "whole number (2-byte)" (Interp.describe (List.nth Interp.blind_plan 2));
  (* a number typed later narrows each of them *)
  check_bool "i32 takes 97" true (Interp.pred_for (List.hd Interp.blind_plan) (v "97") = Some (Eq 97L))
```

In `test_helper_conn.ml`: `snapshot ~cap_mb:(Some 8192) ~budget_mb:None [ ("i32", "i32", 4) ]` sends `{"op":"snapshot","groups":[{"id":"i32","type":"i32","align":4}],"cap_mb":8192}` (no `budget_mb` key, no `pred`), and decodes the reply's groups and stats; an error reply with code `too_big` comes back as that `err`.

- [ ] **Step 2: Run them and see them fail**

Run: `opam exec -- dune test`
Expected: unbound `Interp.blind_plan`.

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run the tests**

Run: `make test-core`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add core && git commit -m "core: the snapshot op and the interpretations a snapshot hunt carries"
```

### Task 6: The snapshot hunt, and kept values that were put back

**Files:**
- Modify: `core/lib/session.ml`, `core/lib/session.mli`, `core/lib/render.ml`, `core/lib/render.mli`, `core/lib/daemon.ml` (only to compile)
- Test: `core/test/test_session.ml`, `core/test/test_render.ml`

**Interfaces:**
- Consumes: `helper.snapshot`, `Interp.blind_plan`.
- Produces:

```ocaml
(* session.mli *)
type note = Fell_back of relation | Showing_wide | Untracked | Lost of int | Snapshot_taken
val create : ?budget_mb:int -> ?snapshot_mb:int -> ?settle:(unit -> unit) -> Helper_api.helper -> t
val snapshot : t -> (outcome, string) result
type kept = { name : string; kaddr : int64; ktag : string; kshown : string; set_to : string option; put_back : bool }
```

- Rules for `snapshot`:
  - It asks the helper for a snapshot with `Interp.blind_plan`'s groups, `cap_mb` from `snapshot_mb` and `budget_mb` as `find` sends it. On success the hunt is those groups, with no baseline, and the outcome carries `Snapshot_taken` and the bytes read.
  - `too_big` from the helper: the hunt there was stays, and the error is `the game has <size> of memory to snapshot, more than moth's limit of <limit>. raise it with MOTH_SNAPSHOT_MB, or hunt with a number: moth find <number>` when the message carries the two sizes (rendered with `Render.size`), else `moth couldn't get the memory for a snapshot. close something large, or hunt with a number: moth find <number>`.
  - Any other failure ends the hunt there was, as a failed `find` does.
  - `next_relation`, `next_number`, `results`, `set`, `keep_row` and `undo` work on a snapshot hunt as on any hunt. With no baseline, a number with no exact match is a dead end.
  - `results` on a snapshot hunt before any step answers `nothing to list yet. tell moth how the value moved first: moth next down, up, same or changed.`
- Rules for texts (`Render`):
  - An outcome with `Snapshot_taken` renders as one line in place of the counts line: `snapshot of <size> taken. change the value in the game, then tell moth how it moved: moth next down, up, same or changed.` then the usual line about unreadable memory when there is some.
  - The dead-end line becomes `nothing left that fits. that step was not applied, so you can try a different number, or hunt by how it moves: moth snapshot`.
  - `too_many` becomes `too many matches to compare by change yet. type the new number instead, or start again by change alone: moth snapshot`.
- Rules for kept values:
  - `keep_row` reads the address now and reports that value (`kept #1 as money (200.00, whole number ×100)`); if the read fails it reports the listing's value as before.
  - A kept value remembers the newest write made to it through `set_kept`: the bytes written and the bytes it replaced. `kept` then gives `set_to = Some <the written value as shown>`, and `put_back = true` exactly when the bytes read now equal the bytes that write replaced and differ from the bytes written. `undo` of that write, `forget`, or losing the game forgets it.
  - `Render.kept` adds to such a line: `you set it to <set_to>; the game put it back.` Other lines are unchanged.

- [ ] **Step 1: Write the failing tests**

```ocaml
let a_snapshot_hunt () =
  let s, f = fresh () in
  queue_snapshot f [ gi "i32" 1000; gi "f32" 1000; gi "i16" 2000; gi "i64" 500; gi "f64" 500; gi "u8" 4000 ] ~bytes:4096;
  let o = ok (Session.snapshot s) in
  check_bool "note" true (List.mem Session.Snapshot_taken o.notes);
  check_str "text" "snapshot of 4 kb taken. change the value in the game, then tell moth how it moved: moth next down, up, same or changed."
    (Render.outcome o);
  check_bool "asked for the blind plan" true
    (last_snapshot_groups f = [ ("i32", "i32", 4); ("f32", "f32", 4); ("i16", "i16", 2); ("i64", "i64", 8); ("f64", "f64", 8); ("u8", "u8", 1) ]);
  check_err "nothing to list before a step" "nothing to list yet. tell moth how the value moved first: moth next down, up, same or changed."
    (Session.results s ~all:false ~from:1 ~limit:20 ~near:None);
  queue_narrow f true [ gi "i32" 3; gi "f32" 2; gi "i16" 5; gi "i64" 0; gi "f64" 0; gi "u8" 9 ];
  let o = ok (Session.next_relation s Session.Down) in
  check_int "main" 10 o.main;
  check_pred "each group asked" Lt_prev (last_narrow_pred f "f32");
  queue_narrow f true [ gi "i32" 1; gi "f32" 0; gi "i16" 1 ];
  ignore (ok (Session.next_number s "97"));
  check_pred "a number after change steps" (Eq 97L) (last_narrow_pred f "i32");
  check_pred "no baseline, so no moved" (Range { lo = 96.; hi = 98.; floor = 1e-6; moved = false }) (last_narrow_pred f "f32")

let a_kept_value_the_game_put_back () =
  let s, f = fresh () in
  queue_read f (Ok (Vint 13L));
  ignore (ok (Session.keep_at s ~addr:0x1e84dfd34L "13" ~name:(Some "conc")));
  queue_write f { prev = Vint 13L; prev_bits = 13L };
  queue_read f (Ok (Vint 20L));                                   (* the check a moment later: still 20 *)
  ignore (ok (Session.set_kept s ~name:"conc" "20"));
  queue_read f (Ok (Vint 20L));
  let k = List.hd (ok (Session.kept s)) in
  check_bool "not put back yet" false k.put_back;
  check_bool "set_to" true (k.set_to = Some "20");
  queue_read f (Ok (Vint 13L));                                   (* later: the game recalculated *)
  let k = List.hd (ok (Session.kept s)) in
  check_bool "put back" true k.put_back;
  let line = Render.kept [ k ] in
  check_bool "line says so" true (String.ends_with ~suffix:"you set it to 20; the game put it back." line);
  check_bool "and still starts with the name" true (String.starts_with ~prefix:"conc" line)
```

(Helper spellings follow the test files' existing helpers; the assertions and values are binding.)

Also one test each:
- `too_big` with the message `4516605952 to snapshot, limit 1073741824`: the error text with `4.2 gb` and `1 gb`, and a hunt in progress is still there (`Session.current` unchanged);
- `too_big` with the message `could not get memory for the snapshot`: the second error text;
- another helper error on `snapshot` ends the hunt there was;
- `Session.create ~snapshot_mb:2048` sends `cap_mb = Some 2048`; without it `None`;
- a `find` after a snapshot hunt replaces it (the scan is sent with `keep:false`), and kept values survive both;
- a kept value read as 200.85 after being set to 200.00 from 106.98 is not `put_back`; after `undo`, `set_to` is `None`;
- `keep_row` reports the value read now (a listing row showing 106.98 whose address reads 200.00 gives `kept #1 as money (200.00, whole number ×100)`), and the listing's value when the read fails;
- the dead-end and too-many texts.

- [ ] **Step 2: Run them and see them fail**

Run: `opam exec -- dune test`
Expected: unbound `Session.snapshot`.

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run the tests**

Run: `make test-core`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add core && git commit -m "core: a hunt from a snapshot, and kept values say when the game put them back"
```

### Task 7: The snapshot command

**Files:**
- Modify: `core/lib/daemon.ml`, `core/lib/daemon.mli`, `core/bin/main.ml`, `docs/dev.md`
- Test: `core/test/test_daemon.ml`

**Interfaces:**
- Consumes: Task 6.
- Produces:
  - Command `snapshot` (no arguments; `snapshot takes no arguments` otherwise). Result and text as `find` gives, from `Session.snapshot`; `"snapshot"` appears in `notes`.
  - Help line, after `find`: `snapshot                             start a hunt with no number: remember everything, then narrow by how the value moves`.
  - `kept --json` items carry `set_to` and `put_back`; the text is `Render.kept`'s.
  - `MOTH_SNAPSHOT_MB`: read once when the daemon starts, like `MOTH_BUDGET_MB`. A whole number from 1 to 1048576 is passed to every session as `~snapshot_mb`; anything else is ignored with one line in `core.log`; empty counts as unset. Documented in `docs/dev.md` with the default (8192) and what a snapshot costs in memory (as much as the game's writable memory, held by the helper until the hunt has narrowed).

- [ ] **Step 1: Write the failing tests**

In `test_daemon.ml`, one test each:
- `snapshot` then `next down` then `results`: the helper saw a `snapshot` with the blind plan, the first reply's text is the snapshot line and its JSON `notes` contains `"snapshot"`, the event log has one event per command;
- `snapshot extra` is the usage error and the helper is not called;
- the helper answering `too_big` with two sizes: the reply is the error text from Task 6, and a following `status` still shows the hunt there was;
- `kept --json` after a `set` that the game later put back: `"set_to":"20","put_back":true`, and the text line ends `you set it to 20; the game put it back.`;
- a daemon started with `MOTH_SNAPSHOT_MB=2048` sends `cap_mb = Some 2048` on `snapshot`; with `abc`, `0` or empty, `None`.

- [ ] **Step 2: Run them and see them fail**

Run: `opam exec -- dune test`
Expected: FAIL (`unknown command: snapshot`).

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run every unit test and build**

Run: `make test && make build && bin/moth help`
Expected: all pass; the help lists `snapshot`.

- [ ] **Step 5: Commit**

```bash
git add core docs/dev.md && git commit -m "moth: snapshot, and kept shows what the game put back"
```

### Task 8: The bottle scenarios

**Files:**
- Modify: `tests/target/target.cpp`, `core/integration/target_ctl.ml`, `core/integration/it_main.ml`, `docs/dev.md` (the suite's running time)

**Interfaces:**
- Produces, in the target: `stamina` (`float` 62.5), a value the scenarios never type; and `--filler-half-mb N`, N MiB of `float` 0.5 (reported to `Target_ctl.start ?filler_half_mb`).
- Uses the existing `total` (`base + bonus`, rewritten every 20 ms), `points` and `--filler-mb`.

- [ ] **Step 1: Add the scenarios**

| # | name | steps and checks |
| --- | --- | --- |
| 25 | a value with no number | `snapshot`; set `stamina` to 50; `next down`; set it to 55; `next up`; `next same`; `results --all -n 200`: `stamina`'s address is listed with tag `decimal`. `set` that row to 99 and the target reports `stamina` 99. |
| 26 | same first | `snapshot`; `next same` (nothing was changed); set `stamina` to 40; `next down`; `results --all -n 200` lists `stamina`'s address. |
| 27 | snapshot speed | target with `--filler-mb 1024`. Print `snapshot: <x> s/GB` and `next same: <y> s/GB` from the helper's own times; both must be under 3.0. |
| 28 | a snapshot over the limit | daemon started with `MOTH_SNAPSHOT_MB=16`, target with `--filler-mb 64`. `find 3` first; `snapshot` exits 1 with a text starting `the game has` and containing `more than moth's limit of 16 mb`; `status --json` still shows the `find 3` hunt. |
| 29 | zero and one, fast | target with `--filler-half-mb 64` (about 16.7 million floats of 0.5). `find 1`: the `f32` group counts over 16,000,000 and `has_values` is true. Set `points` to 0; `next 0`: under 3 seconds end to end, the `f32` group counts under 1,000 afterwards (the unchanged halves were dropped because they had not moved), and `results --near <points addr>` has `points`' own address first. |
| 30 | put back | `keep <total addr> 6 total`; `set total 9`: `held` is false. Wait until the target reports `total` 6; `kept --json`: `total` has `"set_to":"9"` and `"put_back":true`. |

- [ ] **Step 2: Extend the target and `Target_ctl`**

- [ ] **Step 3: Run the scenarios in the scratch bottle**

Run: `make helper && make test-integration` (twice)
Expected: scenarios 1 to 11 and 13 to 30 `ok`, 12 the documented skip; the scan line and both snapshot lines under 3.0 s/GB; `ps axo pid,args | grep -iE "moth daemon|moth-(helper|target)"` shows nothing afterwards. If scenario 9 or 29's expectations about `untracked` or "too many matches" change because groups now keep values, update them under the Global Constraints' rule and say so.

- [ ] **Step 4: Commit**

```bash
git add tests core docs/dev.md && git commit -m "integration: a value with no number, same first, snapshot speed, a snapshot over the limit, zero and one, put back"
```

### Task 9: Prove it on Disco Elysium

This task needs Chris at the keyboard. Do not mark stage 1c done without it. No script outside `moth` may read or write the game during it, and no save file may be read to learn a number.

**Files:**
- Modify: `docs/verification/disco-elysium.md` (add a dated "stage 1c" section)

- [ ] **Step 1: Attach.** `bin/moth attach --bottle "Disco Elysium" disco.exe` with a save loaded. Note the helper's memory before and after the first snapshot (`ps -o rss` of the `moth-helper.exe` process), and ask Chris whether the game stays smooth while the snapshot is held.
- [ ] **Step 2: A thought's research time, by change alone.** Chris starts researching a thought. `moth snapshot`; he lets a game minute or two pass; `moth next down`; repeat until a few rows are left. Read the remaining minutes off `moth results` (they should match the thought's percentage), `keep` it, `set` it to 1; the thought completes on the next game minute.
- [ ] **Step 3: Zero and one.** With one skill point: `find 1`, Chris spends it, `next 0`. Record both times and the counts.
- [ ] **Step 4: Put back.** Conceptualization as in stage 1b (`find`, shirt off, `next`, shirt on, `next`), `keep` the row, `set` it to 20, Chris reopens the character sheet, `moth kept`.
- [ ] **Step 5: Record and judge.** For every step: time, matches per tag, lost, unreadable, and the snapshot's size. The bar:
  - the snapshot, and each step taken while it is held, at 3.0 s/GiB or better;
  - the thought's remaining time found and changed with no number typed until the value was on moth's own screen, and with no save file read;
  - `find 1` and `next 0` each in 8 seconds or less;
  - `moth kept` says the game put Conceptualization back;
  - the game stayed playable while the snapshot was held (Chris's word).
  If a bar is missed, record it as missed with the numbers; do not adjust the bar. If the helper cannot hold a snapshot of the game at all, stop and take that back to Chris.
- [ ] **Step 6: Commit**

```bash
git add docs/verification/disco-elysium.md && git commit -m "verification: disco elysium, stage 1c"
```
