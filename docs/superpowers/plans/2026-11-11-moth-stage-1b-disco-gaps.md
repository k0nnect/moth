# moth stage 1b (the gaps Disco Elysium exposed) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On Disco Elysium, find and change experience, money and skill points with `moth` commands alone, with every step of a hunt for 0 or 1 taking seconds, and be told plainly when a write did not stick.

**Architecture:** No new processes. The helper's range comparison learns to ignore values too small to be what was typed and to require that a value moved; its candidate lists can be read starting from an address. The core keeps found values by name beside the one hunt, lists results in pages and by nearness to a kept value, and checks each write a moment later. The daemon and the `moth` command expose these.

**Tech Stack:** As stage 1: OCaml 5.5.x, dune, yojson, alcotest; C++20, CMake, mingw-w64, doctest, nlohmann/json.

**Spec:** `docs/superpowers/specs/2026-11-11-moth-design.md` (sections 4, 6.1, 6.3, 6.4). The findings this plan answers are in `docs/verification/disco-elysium.md`, "problems found". Read both before starting any task. The helper protocol table is in `docs/superpowers/plans/2026-11-11-moth-stage-1-engine.md`, "Protocols"; this plan's "Protocol changes" adds to it.

## Global Constraints

- Everything in the stage 1 plan's Global Constraints still holds: OCaml and C++ only; SIP on, no `sudo`, CrossOver untouched; memory read and written from outside only; the user never supplies a data type on any command; lowercase plain user-facing text, no emojis; local git only, never a remote; lowercase commit messages with no attribution trailers; no edits to global configuration.
- One hunt at a time (spec 6.3). Values found earlier stay reachable by being kept, not by running several hunts.
- Every protocol addition is an optional field. A request without it behaves exactly as before, and every existing test keeps passing unchanged unless this plan names it.
- Automated tests use only the `uhmm` bottle and a temporary `MOTH_HOME`, and leave no `moth daemon`, `moth-helper` or `moth-target` process running.
- The first-scan bar stays 3.0 s/GiB. Nothing here may slow a scan whose ranges carry no floor.
- All paths are relative to `~/Documents/moth`.

## Review Focus

1. **Typing 0, or 0.00.** Floats that are exactly zero (and negative zero) must still match while tiny ones do not. Pinned in Tasks 1 and 3.
2. **A kept value whose memory goes away.** `moth kept` shows `?` for a value it cannot read; when the game closes, the kept list is cleared with the usual game-closed message; nothing crashes or hangs. Pinned in Task 5.
3. **A value that changes by itself between the write and the re-check** (a timer, a regenerating bar). It must not be called "worked out from something else". Pinned in Task 4.
4. **Names that look like something else.** `moth keep #2 5`, `moth keep #2 0x10`, a name with capitals or spaces, a name already taken. Each is refused with a plain message and nothing is kept. Pinned in Tasks 5 and 6.
5. **Paging off the end.** `results --from` past the last row, `-n 0`, `-n 99999`, `--near` a name that does not exist, `--near` a value with nothing near it. Plain messages. Pinned in Tasks 5 and 6.

## Not in this plan

- Hunting with no number to type (change-only, from a snapshot). It was pencilled in as stage 1b; it is a separate subsystem and gets its own plan (stage 1c) before stage 2.
- Finding the parts a recalculated number is built from, and anything else that maps structure beyond "rows near a kept value". Stage 3.
- Saving kept values to disk, freezing a value. Stage 4.
- Running a narrowing step on several threads. Only if Task 8's time bar fails; then the executor rules on it.

## Protocol changes

**Helper protocol**

- `P` range gains two optional fields: `{"op":"range","lo":0.0,"hi":2.0,"floor":0.000001,"moved":true}`.
  - `floor` (a number above 0): the value matches only if it is exactly zero or its magnitude is at least `floor`.
  - `moved` (`true`): narrow only. The value must also differ from the group's stored previous value. A group without stored values answers `no_values`; in a `scan` it is `bad_request`.
- `peek` takes `offset` or `from`, not both: `{"op":"peek","group":"i32","from":"0x6c551000","limit":64}` returns the items at the group's positions with address at or above `from`, in address order.
- `peek` (with `offset` or with `from`) takes an optional `"holes":true` (ruling R14, from the final review). With it, a position whose value cannot be read is returned in its place as `{"addr":"0x...","v":null}` instead of being left out, so a page holds exactly `limit` positions unless the group ends. Without the field the reply is exactly as before. `target_gone` is unchanged. The core always sends it; its rows are numbered by position, and a row whose value cannot be read shows `?`.
- `scan`'s existing `budget_mb` is now sent by the core when one is configured.

**Client protocol**

- `results` result: `{"rows":[{"n","addr","interp_id","tag","shown","away":int|null}],"total":int,"from":int}`. It was a bare list. A row whose value cannot be read has `"shown":"?"`.
- `set` result: `{"message":"<text>","held":true|false|null}`. `null` (ruling R15, from the final review): the read-back could not happen, and the text's second line says why.
- Each line of `events.jsonl` gains `"opts"`, an object of the options given (when at least one was; `--json` is not one), and a `set`'s line gains `"held"` as in its result.
- `kept` result: `[{"name","addr","tag","shown"}]`.
- `status` result gains `"closed":"<name>"` when it found that the attached game had exited.

## File Structure

```
helper/engine/pred.hpp .cpp        Pred: floor, moved
helper/engine/block_scan.cpp       range loop honours floor
helper/engine/posset.hpp .cpp      lower_bound
helper/engine/engine.hpp .cpp      peek_from; freed ranges skipped
helper/proto/handler.cpp           floor, moved, peek from
core/lib/helper_api.ml             range record; scan budget; peek_from
core/lib/wire.ml .mli              the new fields
core/lib/interp.ml .mli            tiny, holds
core/lib/helper_conn.ml            budget_mb, peek from
core/lib/session.ml .mli           moved, baseline, write check, pages, near, keep
core/lib/render.ml .mli            away column, kept rows
core/lib/daemon.ml, client.ml, core/bin/main.ml   commands and options
tests/target/target.cpp            player block, derived total, tiny filler
core/integration/it_main.ml, target_ctl.ml         scenarios 17 to 24
docs/dev.md                        MOTH_BUDGET_MB
docs/verification/disco-elysium.md stage 1b section (Task 8)
```

---

### Task 1: The helper's range comparison: floor and moved

**Files:**
- Modify: `helper/engine/pred.hpp`, `helper/engine/pred.cpp`, `helper/engine/block_scan.cpp`, `helper/engine/engine.cpp`, `helper/proto/handler.cpp`, `helper/test/test_util.hpp`
- Test: `helper/test/test_block_scan.cpp`, `helper/test/test_engine_narrow.cpp`, `helper/test/test_handler.cpp`

**Interfaces:**
- Produces: `struct Pred { ...; double floor = 0; bool moved = false; }`. `matches()` for `Range`: `lo < v && v < hi`, and (`floor <= 0` or `v == 0` or `|v| >= floor`), and (not `moved` or the value differs from `prev` by the same comparison `NePrev` uses). Test helper `inline Pred range_floor(double lo, double hi, double floor)` in `test_util.hpp`.
- Produces: the JSON fields in "Protocol changes". `floor` must be a number above 0 and `moved` a boolean, else `bad_request`.

- [ ] **Step 1: Write the failing tests**

```cpp
// test_block_scan.cpp
TEST_CASE("a range with a floor skips tiny values and keeps exact zero") {
  Buf b(64);
  b.put<float>(0, 1.0f); b.put<int32_t>(4, 7);   // 7 read as a float is about 1e-44
  b.put<float>(8, 1e-7f); b.put<float>(12, 2e-6f); b.put<float>(16, 0.0f); b.put<float>(20, -0.0f);
  b.put<float>(24, -1e-7f); b.put<float>(28, -0.5f);
  CHECK(run(b, 32, 0, {G("f", F32, 4, 0, range_floor(0, 2, 1e-6))})[0] == V{0, 12});
  CHECK(run(b, 32, 0, {G("f", F32, 4, 0, range_floor(-1, 1, 1e-6))})[0] == V{12, 16, 20, 28});
  CHECK(run(b, 32, 0, {G("f", F32, 4, 0, range(0, 2))})[0] == V{0, 4, 8, 12});   // no floor: as before
}

// test_engine_narrow.cpp
TEST_CASE("a moved range drops values that pass the range without having changed") {
  FakeMemory m; m.add_region(0x10000, 4096); m.poke<float>(0x10000, 5.0f); m.poke<float>(0x10010, 5.5f);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("f", F32, 4, 0, range(4, 6))}, false, s); CHECK(e.groups()[0].count == 2);
  m.poke<float>(0x10000, 6.0f);
  Pred p = range(5, 7); p.moved = true;
  CHECK(e.narrow({{"f", p}}, true, g, s)); CHECK(g[0].count == 1); CHECK(e.peek("f", 0, 9)[0].addr == 0x10000);
}
TEST_CASE("moved needs stored values and cannot start a scan") {
  FakeMemory m; three(m); Engine e(m, {.value_cap = 2}); ScanStats s; std::vector<GroupInfo> g;
  Pred p = range(99, 101); p.moved = true;
  CHECK_THROWS_WITH_AS(e.scan({G("a", I32, 4, 0, p)}, false, s), doctest::Contains("bad_request"), EngineError);
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  CHECK_THROWS_WITH_AS(e.narrow({{"a", p}}, true, g, s), doctest::Contains("no_values"), EngineError);
}
```

Also:
- Extend the existing test that checks the range loop against `matches()` value by value so it also runs with floor `1e-6` for `F32` and `F64` over the ranges (0, 2), (-1, 1), (-2, 0) and (5, 7).
- In `test_handler.cpp`, `TEST_CASE("range floor and moved on the wire")`: a region with `float 1.0` at `0x10000` and `int32 7` at `0x10004`; a scan with `{"op":"range","lo":0.0,"hi":2.0,"floor":0.000001}` counts 1 and without `floor` counts 2; a scan with `"moved":true` is `bad_request`; `"floor":-1` and `"moved":"yes"` are `bad_request`.

- [ ] **Step 2: Run them and see them fail**

Run: `make test-native`
Expected: compile errors for `range_floor` and `Pred::moved`.

- [ ] **Step 3: Implement**

`scan_range` keeps comparing integer keys with no floating point in the loop. With a floor, the matching values are at most three key intervals: (lo, -floor], the two zero keys (which are adjacent), and [floor, hi). With no floor, or a range that does not reach inside (-floor, floor), the loop must be the single-interval loop it is today. In `engine.cpp`, `validate` refuses `moved` in a scan and `compares_with_last` is true for a `moved` range.

- [ ] **Step 4: Run the tests and the native bench**

Run: `make test-native && make bench-native`
Expected: all test cases pass; the bench's scan figure is within noise of its figure before this task (run it before starting and note both in the report).

- [ ] **Step 5: Commit**

```bash
git add helper && git commit -m "helper: a range can ignore tiny values and require that a value moved"
```

### Task 2: The helper reads candidates from an address, and skips freed ranges

**Files:**
- Modify: `helper/engine/posset.hpp`, `helper/engine/posset.cpp`, `helper/engine/engine.hpp`, `helper/engine/engine.cpp`, `helper/proto/handler.cpp`
- Test: `helper/test/test_engine_scan.cpp`, `helper/test/test_engine_narrow.cpp`, `helper/test/test_handler.cpp`

**Interfaces:**
- Produces: `uint64_t PositionSet::lower_bound(uint64_t addr) const;` the index of the first position at or above `addr`, or `count()`.
- Produces: `std::vector<Item> Engine::peek_from(const std::string& id, uint64_t addr, uint64_t limit);` same rules and errors as `peek`.
- Produces: `peek` with `from` on the wire (see "Protocol changes"); both `offset` and `from`, or neither, is `bad_request`.
- Changes: when a read fails during a scan or a narrow, the step asks `writable_regions()` once and treats bytes outside every listed region as unreadable (scan) or lost (narrow) without reading them. Bytes inside a listed region are still re-read in halves as now.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("lower_bound finds the first position at or after an address") {
  PositionSet s(4); s.add_block(0x1000, 4096, V{0, 8, 100}); s.add_block(0x9000, 4096, V{4});
  CHECK(s.lower_bound(0) == 0); CHECK(s.lower_bound(0x1008) == 1); CHECK(s.lower_bound(0x1009) == 2);
  CHECK(s.lower_bound(0x1065) == 3); CHECK(s.lower_bound(0x9004) == 3); CHECK(s.lower_bound(0x9005) == 4);
}
TEST_CASE("peek_from starts at an address") {
  FakeMemory m; three(m); Engine e(m); ScanStats s;
  e.scan({G("a", I32, 4, 0, eq(100))}, false, s);
  auto items = e.peek_from("a", 0x10004, 9); CHECK(items.size() == 2); CHECK(items[0].addr == 0x10010);
  CHECK(e.peek_from("a", 0x20000, 9).empty());
}
TEST_CASE("a freed range is skipped without a read for every page") {
  const uint64_t big = 16u << 20;
  FakeMemory m; m.set_all_or_nothing(true);
  m.add_region(0x100000, big); m.add_region(0x100000 + big, 4096);
  for (uint64_t a = 0x100000; a < 0x100000 + big; a += 4096) m.poke<int32_t>(a, 7);
  m.poke<int32_t>(0x100000 + big, 7);
  Engine e(m); ScanStats s; std::vector<GroupInfo> g;
  e.scan({G("a", I32, 4, 0, eq(7))}, false, s); CHECK(e.groups()[0].count == 4097);
  m.remove_region(0x100000);   // the game frees the whole 16 MiB
  m.read_calls = 0;
  CHECK(e.narrow({{"a", eq(7)}}, true, g, s)); CHECK(g[0].count == 1); CHECK(s.lost == 4096); CHECK(m.read_calls <= 16);
}
```

Also: the `lower_bound` answers hold for a block dense enough to be stored as a bitmap (build one with every aligned offset set and check three addresses); a handler test that `peek` with `"from":"0x10004"` returns the item at `0x10010` first, and that `offset` together with `from` is `bad_request`.

- [ ] **Step 2: Run them and see them fail**

Run: `make test-native`
Expected: compile errors for `lower_bound` and `peek_from`.

- [ ] **Step 3: Implement the three changes**

- [ ] **Step 4: Run the tests**

Run: `make test-native`
Expected: all pass, including the stage 1 tests for a freed page inside a listed region.

- [ ] **Step 5: Commit**

```bash
git add helper && git commit -m "helper: peek from an address, and freed ranges cost no read per page"
```

### Task 3: The core's types for the new fields

**Files:**
- Modify: `core/lib/helper_api.ml`, `core/lib/wire.ml`, `core/lib/wire.mli`, `core/lib/interp.ml`, `core/lib/interp.mli`, `core/lib/helper_conn.ml`, `core/lib/session.ml` (only to compile against the new `Range`), `core/test/fake_helper.ml`
- Test: `core/test/test_interp.ml`, `core/test/test_wire.ml`, `core/test/test_helper_conn.ml`

**Interfaces:**
- Produces, in `Helper_api`:
  - `type range = { lo : float; hi : float; floor : float; moved : bool }` and `pred = Eq of int64 | Range of range | Lt_prev | Gt_prev | Eq_prev | Ne_prev`
  - `scan : keep:bool -> budget_mb:int option -> group_spec list -> (group_info list * stats, err) result`
  - `peek_from : id:string -> vtype:string -> addr:int64 -> limit:int -> (item list, err) result`
- Produces, in `Interp`:
  - `val tiny : float` (1e-6)
  - `pred_for`: the float readings (plain `f32`/`f64` and the ÷100 reading) carry `floor = tiny` when the number was typed with at most 4 decimal places, else `floor = 0.`; `moved = false` always.
  - `val holds : t -> Value.t -> Helper_api.value -> bool`: the value is one `pred_for t v` would match (false when `pred_for` is `None`; `moved` ignored).
- Produces, in `Wire.pred_to_json`: `floor` is written only when above 0, `moved` only when true.
- Produces, in `Fake_helper`: `Scan of { keep; budget_mb; specs }`, `Peek_from of { id; vtype; addr; limit }`, `Read of { addr; vtype }`, with reply queues `queue_peek_from` and `queue_read` in the style of the existing ones.

- [ ] **Step 1: Write the failing tests**

```ocaml
(* test_interp.ml *)
let v s = Result.get_ok (Value.parse s)
let interp id n = List.find (fun (i : Interp.t) -> i.id = id) (Interp.plan (v n))

let floor_on_float_readings () =
  let p id n = Interp.pred_for (interp id n) (v n) in
  check_bool "f32 of 1" true (p "f32" "1" = Some (Range { lo = 0.; hi = 2.; floor = 1e-6; moved = false }));
  check_bool "f32 of 0" true (p "f32" "0" = Some (Range { lo = -1.; hi = 1.; floor = 1e-6; moved = false }));
  check_bool "f32pct of 1" true (p "f32pct" "1" = Some (Range { lo = 0.; hi = 0.02; floor = 1e-6; moved = false }));
  check_bool "six decimals: no floor" true (match p "f32" "0.000002" with Some (Range r) -> r.floor = 0. | _ -> false)

let holds () =
  check_bool "i32 15" true (Interp.holds (interp "i32" "15") (v "15") (Vint 15L));
  check_bool "i32 16" false (Interp.holds (interp "i32" "15") (v "15") (Vint 16L));
  check_bool "85.3 shows as 85" true (Interp.holds (interp "f32" "85") (v "85") (Vfloat 85.3));
  check_bool "a tiny float is not 1" false (Interp.holds (interp "f32" "1") (v "1") (Vfloat 1e-30));
  check_bool "zero is 0" true (Interp.holds (interp "f32" "0") (v "0") (Vfloat 0.));
  check_bool "cents" true (Interp.holds (interp "i32x100" "5.32") (v "5.32") (Vint 532L))
```

Also: in `test_wire.ml`, a range with `floor = 0.` and `moved = false` encodes to an object with exactly the keys `op`, `lo`, `hi`; with `floor = 1e-6` and `moved = true` it also has `floor` (a float equal to 1e-6) and `moved` (`true`). In `test_helper_conn.ml`, `scan ~budget_mb:(Some 64)` sends `"budget_mb":64` and `None` sends no such key; `peek_from` sends `"from"` as a `0x` address and no `offset`.

- [ ] **Step 2: Run them and see them fail**

Run: `opam exec -- dune test`
Expected: type errors on `Range`.

- [ ] **Step 3: Implement**

Every existing use of `Range (a, b)` becomes a record with `floor` from the rule above and `moved = false`. Existing tests that build a `Range` are updated to the record; their expectations for float readings gain `floor = 1e-6`.

- [ ] **Step 4: Run the tests**

Run: `make test-core`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add core && git commit -m "core: ranges carry a floor and a moved flag, scans a budget, peeks an address"
```

### Task 4: The hunt: moved floats, a cleared baseline, a checked write, a budget

**Files:**
- Modify: `core/lib/session.ml`, `core/lib/session.mli`, `core/lib/daemon.ml` (only to compile against the new `set` result)
- Test: `core/test/test_session.ml`

**Interfaces:**
- Consumes: Task 3's `Helper_api`, `Interp.tiny`.
- Produces:
  - `val create : ?budget_mb:int -> ?settle:(unit -> unit) -> Helper_api.helper -> t` (`settle` defaults to a 0.25 second sleep)
  - `type set_done = { text : string; w : written; held : bool }`
  - `val set : t -> row:int -> string -> (set_done, string) result`
- Behaviour:
  - `find` sends `budget_mb` on its scan. Scans with `keep:true` send `None`.
  - `next_number`, exact step: when the typed number differs from the last number that applied, a range sent to a group with stored values has `moved = true`. Same number again, no baseline, or a group without values: `moved = false`. (Replaced by ruling R13, from the final review: `moved` is sent only when there is a baseline, the group has stored values, the new number's and the baseline number's predicates for the group are both ranges, and neither range contains the other. So 5 then 6 sends it; 5 then 5.3, and 5.3 then 5, do not. Ruling R12: a fallback step clears the baseline as a relation step does.)
  - A successful `set` or `undo` clears the fallback baseline, as a relation step does.
  - After a successful write, `set` calls `settle ()`, reads the address back and compares raw bytes with what it wrote:
    - same: `held = true`, text unchanged from stage 1.
    - the bytes it replaced: `held = false`, a second line `the game put it straight back to <prev>. it's worked out from something else, so writing here won't stick.`
    - anything else: `held = false`, a second line `it already reads <value>: the game keeps changing it.`
    - the read fails: `held = true` and no second line. (Replaced by ruling R15, from the final review: `held` is `bool option`, and a read-back that could not happen is `None` with a second line saying why: `the game closed right after, so moth couldn't check it.` (`target_gone`), `moth lost contact with its helper right after, so it couldn't check it.` (`helper_lost`), or `moth couldn't read it back to check.`. Losing the game or the helper there clears the hunt, kept values and undo list. The write is reported and logged in every case.)
  - The write stays on the undo list in every case.

- [ ] **Step 1: Write the failing tests**

```ocaml
let new_number_asks_floats_to_have_moved () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 3; gi "f32" 2; gi ~values:false "f64" 9 ];
  ignore (ok (Session.find s "5"));
  queue_narrow f true [ gi "i32" 1; gi "f32" 1; gi ~values:false "f64" 9 ];
  ignore (ok (Session.next_number s "6"));
  check_pred "f32" (Range { lo = 5.; hi = 7.; floor = 1e-6; moved = true }) (last_narrow_pred f "f32");
  check_pred "f64 has no values" (Range { lo = 5.; hi = 7.; floor = 1e-6; moved = false }) (last_narrow_pred f "f64");
  check_pred "i32" (Eq 6L) (last_narrow_pred f "i32");
  queue_narrow f true [ gi "i32" 1; gi "f32" 1 ];
  ignore (ok (Session.next_number s "6"));
  check_pred "the same number again" (Range { lo = 5.; hi = 7.; floor = 1e-6; moved = false }) (last_narrow_pred f "f32")

let a_write_the_game_puts_back () =
  let h, f = Fake_helper.create () in
  let s = Session.create ~settle:(fun () -> ()) h in
  queue_scan f [ gi "i32" 1 ];
  ignore (ok (Session.find s "6"));
  queue_peek f "i32" [ it 0x10L (Vint 6L) ];
  ignore (ok (Session.results s ~all:false));
  queue_write f { prev = Vint 6L; prev_bits = 6L };
  queue_read f (Ok (Vint 6L));
  let d = ok (Session.set s ~row:1 "9") in
  check_bool "held" false d.held;
  check_str "text"
    "set #1 to 9 (was 6)\nthe game put it straight back to 6. it's worked out from something else, so writing here won't stick."
    d.text
```

(`Session.results` keeps its stage 1 signature until Task 5; use whatever it is when this task runs. Add `last_narrow_pred f id`, `show_pred` for the record and `queue_read` wherever the file's helpers lack them.)

Also, one test each:
- the read-back equals what was written: `held` true and the text is the single stage 1 line;
- the read-back is 7: `held` false and the second line is `it already reads 7: the game keeps changing it.`;
- the read-back fails with `read_failed`: `held` true, one line;
- `find 61`, `results`, `set #1 999`, then `next 995` with the helper answering "not applied": the outcome is a dead end and exactly one narrow was sent (no fallback narrow);
- `Session.create ~budget_mb:64`: the `find` scan carries `Some 64` and a later `keep:true` rescan carries `None`.

- [ ] **Step 2: Run them and see them fail**

Run: `opam exec -- dune test`
Expected: FAIL (unbound `set_done`, `~settle`).

- [ ] **Step 3: Implement in `session.ml`**

- [ ] **Step 4: Run the tests**

Run: `make test-core`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add core && git commit -m "core: a new number means floats moved, a set clears the baseline and is checked a moment later"
```

### Task 5: The hunt: pages, nearness, kept values

**Files:**
- Modify: `core/lib/session.ml`, `core/lib/session.mli`, `core/lib/daemon.ml` (only to compile)
- Test: `core/test/test_session.ml`

**Interfaces:**
- Consumes: `Interp.holds`, `helper.peek_from`, `helper.read`, `set_done`.
- Produces:

```ocaml
type row = { n : int; addr : int64; interp_id : string; tag : string; shown : string; away : int option }
type listing = { rows : row list; total : int; from : int }
val reach : int                      (* 4096: how far --near looks, in bytes, each way *)
val results : t -> all:bool -> from:int -> limit:int -> near:string option -> (listing, string) result
type kept = { name : string; kaddr : int64; ktag : string; kshown : string }
val keep_row : t -> row:int -> name:string option -> (string, string) result
val keep_at : t -> addr:int64 -> string -> name:string option -> (string, string) result
val kept : t -> (kept list, string) result
val forget : t -> string -> (string, string) result
val set_kept : t -> name:string -> string -> (set_done, string) result
```

- Rules for `results` without `near`:
  - Groups shown are the tracked ones with matches in the tier being shown (main; wide too under `all` or when only wide is left). The 1,000-match cut-off is gone.
  - Group order: fewest matches first, ties in plan order. Within a group, address order.
  - Rows are numbered from 1 across that order. `total` is the sum of the shown groups' counts. `from` is the first row wanted (1-based); `limit` rows at most are returned.
  - `from` above `total` (and `total` above 0): `no result #<from>. there are <total>.`
  - `set` and `keep_row` accept any row of the latest listing, until the hunt changes.
- Rules for `results ~near:(Some target)`:
  - `target` is a kept name or a `0x` address. Unknown name: `no kept value called <name>. see: moth kept`.
  - Rows are every position within `reach` bytes of the target's address in the shown groups, whatever the group's size. A position at the target's own address is included, with `away` 0. Order: nearest first, ties in plan order. `away` is the signed distance in bytes. Numbered from 1; `total` is their number; `from` and `limit` page through them.
  - None: `nothing within 4 kb of <target>.`
- Rules for kept values:
  - A name is a lowercase letter followed by up to 23 lowercase letters, digits, `_` or `-`. Anything else: `a name is lowercase letters, digits, - and _, starting with a letter.` With no name given, the first unused of `v1`, `v2`, ...
  - A name in use: `there's already a kept value called <name>. pick another, or: moth forget <name>`. At most 64 kept: `that's 64 kept values, the most moth holds. forget one first.`
  - `keep_row`: `kept #<n> as <name> (<shown>, <tag>)`. Unknown row: the stage 1 `no result #<n>. run: moth results`.
  - `keep_at ~addr text`: tries each interpretation of `Interp.plan` for the typed number, in plan order, reading the address as that interpretation's type; the first one that `Interp.holds` wins: `kept 0x<addr> as <name> (<shown>, <tag>)`. None: `nothing at 0x<addr> reads as <text>.`
  - `kept` reads each value now; one that cannot be read has `kshown = "?"`. Order: as kept.
  - `set_kept` behaves as `set` (encode, write, undo list, baseline, check) with `<name>` in place of `#<n>` in the texts, as does `undo` of it (`restored <name> to <value>`).
  - `forget`: `forgot <name>`, or the unknown-name message.
  - Kept values survive `find`. Losing the game or the helper clears them along with the hunt.

- [ ] **Step 1: Write the failing tests**

```ocaml
let small_groups_come_first_and_pages_continue () =
  let s, f = fresh () in
  queue_scan f [ gi "i32" 30; gi "f32" 0; gi "i64" 2 ];
  ignore (ok (Session.find s "5"));
  queue_peek f "i64" [ it 0x100L (Vint 5L); it 0x200L (Vint 5L) ];
  queue_peek f "i32" [ it 0x10L (Vint 5L); it 0x14L (Vint 5L); it 0x18L (Vint 5L) ];
  let l = ok (Session.results s ~all:false ~from:1 ~limit:5 ~near:None) in
  check_int "total" 32 l.total;
  check_ids "order" [ "i64"; "i64"; "i32"; "i32"; "i32" ] (List.map (fun (r : Session.row) -> r.interp_id) l.rows);
  check_bool "the i32 page asked for" true (last_peek f "i32" = (0, 3));   (* offset, limit *)
  queue_peek f "i32" [ it 0x1cL (Vint 5L) ];
  let l = ok (Session.results s ~all:false ~from:6 ~limit:1 ~near:None) in
  check_int "numbered on" 6 (List.hd l.rows).n;
  check_bool "offset 3" true (last_peek f "i32" = (3, 1));
  check_err "past the end" "no result #33. there are 32." (Session.results s ~all:false ~from:33 ~limit:5 ~near:None)

let near_a_kept_value () =
  let s, f = fresh () in
  queue_read f (Ok (Vint 15L));
  check_str "kept" "kept 0x6c551068 as xp (15, whole number)" (ok (Session.keep_at s ~addr:0x6c551068L "15" ~name:(Some "xp")));
  queue_scan f [ gi "i32x100" 5093; gi "i64x100" 304 ];
  ignore (ok (Session.find s "5.32"));
  queue_peek_from f "i64x100" [ it 0x6c551078L (Vint 532L); it 0x6c559000L (Vint 532L) ];
  queue_peek_from f "i32x100" [ it 0x6c551078L (Vint 532L); it 0x6c559000L (Vint 532L) ];
  let l = ok (Session.results s ~all:false ~from:1 ~limit:20 ~near:(Some "xp")) in
  check_int "two rows, both at the money address" 2 l.total;
  check_bool "away" true (List.for_all (fun (r : Session.row) -> r.addr = 0x6c551078L && r.away = Some 16) l.rows);
  check_bool "asked from reach below" true (last_peek_from_addr f "i32x100" = Int64.sub 0x6c551068L 4096L)
```

(`0x6c559000` is beyond `reach`, so it is dropped and the group's paging stops there. Add `last_peek`, `last_peek_from_addr` to the helpers if missing.)

Also, one test each:
- `keep_row` after a listing, then a new `find`: `kept` still lists it, with its value read through `helper.read`;
- `keep_at` where the address reads 16 for a typed 15: `nothing at 0x6c551068 reads as 15.`;
- a kept value whose read fails with `read_failed` shows `?`;
- names: `Xp`, `1up`, a 25-letter name are refused with the name message; a second `xp` gets the already-kept message; with no name the first two are `v1` and `v2`;
- `set_kept ~name:"xp" "95"` writes to the kept address as its type and says `set xp to 95 (was 15)`; `undo` says `restored xp to 15`;
- `set_kept` on an unknown name, and `forget` on one, give the unknown-name message;
- a helper error `target_gone` during `kept` returns the game-closed message, and `kept` afterwards (on a fresh attach) is empty;
- `near` an unknown name; `near` with nothing in reach;
- `near` a raw address (`~near:(Some "0x6c551068")`) works without anything kept.

- [ ] **Step 2: Run them and see them fail**

Run: `opam exec -- dune test`
Expected: FAIL (the new signatures do not exist).

- [ ] **Step 3: Implement in `session.ml`**

A `near` listing pages through `peek_from` 64 items at a time per group, starting `reach` below the target and stopping at the first address more than `reach` above it.

- [ ] **Step 4: Run the tests**

Run: `make test-core`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add core && git commit -m "core: results in pages and by nearness, and found values kept by name"
```

### Task 6: Commands for all of it

**Files:**
- Modify: `core/lib/daemon.ml`, `core/lib/daemon.mli`, `core/lib/client.ml`, `core/lib/client.mli`, `core/lib/render.ml`, `core/lib/render.mli`, `core/bin/main.ml`, `docs/dev.md`
- Test: `core/test/test_daemon.ml`, `core/test/test_render.ml`

**Interfaces:**
- Consumes: Tasks 4 and 5.
- Produces these commands (usage text in `main.ml`, one line each, same style):

| command | does |
| --- | --- |
| `results [--all] [-n <rows>] [--from <row>] [--near <name\|0xaddr>]` | `-n` from 1 to 200, default 20 |
| `keep <#n> [name]` / `keep <0xaddr> <number> [name]` | keep a result, or an address that shows a number |
| `kept` | the kept values, read now |
| `forget <name>` | drop a kept value |
| `set <#n\|name> <number>` | as before, or into a kept value |

- Usage errors (from `check_usage`):
  - `results -n needs a number from 1 to 200`
  - `results --from needs a row number, 1 or more`
  - `keep needs a result, or an address and the number it shows: moth keep <#n> [name], or moth keep <0xaddr> <number> [name]`
  - `forget needs a name: moth forget <name>`
  - `set needs a result number or a kept name, and a number: moth set <#n|name> <number>`
- `Client.parse_args` accepts `--from <v>` and `--near <v>` as options with a value.
- Text:
  - `results` prints the rows; a `near` listing has one more column, the distance as `+16` or `-8`. When rows remain: a last line `showing <a>-<b> of <total>. more: moth results --from <b+1>` (with `--near <target>` repeated in it when given).
  - `kept` prints one line per value: name, value, address, tag. None kept: `nothing kept yet. keep a result with: moth keep <#n> [name]`
- JSON: as in "Protocol changes". `set` and `undo` events keep their `write` object.
- `status` asks the helper when something is attached. If the helper reports nothing attached, the daemon drops the target, its hunt, kept values and undo list, and answers `not attached. <name> closed.` with `"attached":null,"hunt":null,"closed":"<name>"`.
- `MOTH_BUDGET_MB`: read once when the daemon starts. A whole number from 1 to 65536 is passed to every session as `~budget_mb`; anything else is ignored with one line in `core.log`. Documented in `docs/dev.md`.

- [ ] **Step 1: Write the failing tests**

In `test_daemon.ml`, with the fake helper, one test each:
- `results --from 21 -n 5` passes `from = 21`, `limit = 5`; the JSON has `rows`, `total`, `from`; the text ends with the `showing ... more:` line when rows remain and has no such line when they do not;
- `results -n 0`, `-n 201`, `--from 0`: the usage errors above, and the helper is not called;
- `keep #1 xp`, `kept`, `set xp 95`, `forget xp` round trip: the texts from Task 5, `kept --json` is a list of `{name, addr, tag, shown}`, the `set` JSON has `"held":true`;
- `keep 0x6c551068 15 xp`: address form reaches `keep_at`;
- `keep #2 5` and `keep #2 0x10` are refused (the name message) and nothing is kept; `keep 15` and `keep` alone give the usage error;
- `set #1 9` where the read-back shows the old value: JSON `"held":false` and the two-line text;
- `status` when the helper's `status` answers `None`: text `not attached. disco.exe closed.`, JSON `closed`, and a following `kept` says not attached;
- a daemon started with `MOTH_BUDGET_MB=64` sends `budget_mb = Some 64` on `find`; with `MOTH_BUDGET_MB=abc` it sends `None`.

In `test_render.ml`: rows with `away = Some 16` and `Some (-8)` print `+16` and `-8`; rows without print as in stage 1.

- [ ] **Step 2: Run them and see them fail**

Run: `opam exec -- dune test`
Expected: FAIL.

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run every unit test and build the command**

Run: `make test && make build && bin/moth help`
Expected: all pass; the help lists `keep`, `kept`, `forget` and the new `results` options.

- [ ] **Step 5: Commit**

```bash
git add core docs/dev.md && git commit -m "moth: results pages and --near, keep, kept, forget, set by name, status notices a closed game"
```

### Task 7: The stand-in target and the bottle scenarios

**Files:**
- Modify: `tests/target/target.cpp`, `core/integration/target_ctl.ml`, `core/integration/it_main.ml`
- Modify: existing scenarios that read `results --json` as a list (it is now `{"rows": ...}`) or `set --json`.

**Interfaces:**
- Produces, in the target (reported with the other variables):
  - a block of five adjacent `int32`: `xp` 15, `level` 2, `points` 1, an unnamed 0, `cents` 532;
  - `base` (`int32` 5), `bonus` (`int32` 1) and `total` (`int32`), with a thread that writes `total = base + bonus` every 20 ms;
  - `charge` (`float` 5.0) and `drift` (`float` 5.5);
  - `--filler-tiny-mb N`: N MiB of `int32` values cycling 1 to 4096.
- Produces: `Target_ctl.start ?filler_tiny_mb`.

- [ ] **Step 1: Add the scenarios (they fail until the target has the new variables)**

| # | name | steps and checks |
| --- | --- | --- |
| 17 | tiny values | target with `--filler-tiny-mb 256`. `find 1`: the `f32` and `f64` groups each count under 1,000. Set `points` to 0, `next 0`: under 3 seconds end to end, and `results --near <points addr>` has `points`' own address as its first row, with `away` 0. |
| 18 | a float that did not move | `find 5`; set `charge` to 6; `next 6`; `results --all -n 200`: `charge`'s address is listed and `drift`'s is not. |
| 19 | a recalculated number | `find 6`; set `bonus` 0; `next 5`; set `bonus` 1; `next 6`; `total`'s address is in `results`; `set` that row to 9: JSON `held` is false, the text's second line starts `the game put it straight back to 6`, and the target still reports `total` 6. |
| 20 | keep, near, set by name | `find 15`; set `xp` 20; `next 20`; `keep #1 xp` (its address is `xp`'s). `find 5.32`; `results --near xp`: the first row is `cents`' address with `away` 16; `set #1 99.99`: the target reports `cents` 9999. `set xp 95`: the target reports `xp` 95, and `kept --json` shows `xp` as `95`. `undo` twice: `xp` 20 and `cents` 532 again. |
| 21 | keep by address | `keep <level addr> 2 lvl`: tag `whole number`. `keep <level addr> 7`: `nothing at 0x… reads as 7.` |
| 22 | pages | target with `--filler-small-mb 1`. `find 3`; `results -n 5`: 5 rows and `total` above 5; `results --from 6 -n 5`: rows numbered 6 to 10, no address shared with the first page; `set #7 4` succeeds. |
| 23 | over budget | daemon started with `MOTH_BUDGET_MB=1`, target with `--filler-small-mb 64`. `find 3`: notes include `untracked`. Set `lives` to 2; `next 2`: `lives`' address is in `results`. |
| 24 | status after the game closes | quit the target; `status --json`: `attached` null and `closed` is the target's name. |

- [ ] **Step 2: Extend the target and `Target_ctl`**

- [ ] **Step 3: Run the scenarios in the scratch bottle**

Run: `make helper && make test-integration`
Expected: scenarios 1 to 11 and 13 to 24 `ok`, 12 the documented skip; the speed line still under 3.0 s/GiB; `ps axo pid,args | grep -iE "moth daemon|moth-(helper|target)"` shows nothing afterwards.

- [ ] **Step 4: Commit**

```bash
git add tests core && git commit -m "integration: tiny values, floats that did not move, a recalculated number, keep and near, pages, over budget, a closed game"
```

### Task 8: Prove it on Disco Elysium

This task needs Chris at the keyboard. Do not mark stage 1b done without it. No script outside `moth` may read or write the game during it.

**Files:**
- Modify: `docs/verification/disco-elysium.md` (add a dated "stage 1b" section; leave the stage 1 record as it is)

- [ ] **Step 1: Attach.** Chris starts the game from CrossOver with his headphones already connected and loads a save. `bin/moth attach --bottle "Disco Elysium" disco.exe`.
- [ ] **Step 2: Experience, by numbers.** `find <xp>`, Chris gains experience, `next <new>`, until one address is left. `keep #1 xp`. `set xp 95`; the next gain levels him up.
- [ ] **Step 3: Money, through its neighbour.** `find <money as shown>`, then `results --near xp`. Set the nearest plausible row to an obvious amount; Chris confirms on screen. `keep` it as `money`.
- [ ] **Step 4: Skill points.** With a skill point in hand: `find 1`, Chris spends it, `next 0`. Record both step times. `results --near xp`, set the nearest row to 5; Chris confirms five on the character sheet.
- [ ] **Step 5: A recalculated number.** Conceptualization as in stage 1 (`find 6`, shirt off, `next 5`, shirt on, `next 6`), then `set` one of the rows. Record what moth said.
- [ ] **Step 6: Record and judge.** For every step: time, matches per tag, lost, unreadable. The bar:
  - first scans at 3.0 s/GiB or better;
  - each step of the skill-point hunt (`find 1`, `next 0`) in 8 seconds or less (stage 1: 26 to 30);
  - experience, money and skill points each found, changed and confirmed using `moth` commands only;
  - the Conceptualization write reported as not held, in plain words.
  If a bar is missed, record it as missed with the numbers; do not adjust the bar.
- [ ] **Step 7: Commit**

```bash
git add docs/verification/disco-elysium.md && git commit -m "verification: disco elysium, stage 1b"
```
