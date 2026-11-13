# developing moth

## toolchains

- OCaml 5.5.2 in a local opam switch (`_opam/`), with dune, yojson and alcotest.
- C++20 with cmake (Makefiles generator, no ninja). Native build uses Apple clang. The Windows helper uses mingw-w64 from Homebrew (`x86_64-w64-mingw32-g++` and `i686-w64-mingw32-g++`).
- Nothing needs sudo. `opam init` runs with `--no-setup`, so no shell profile is touched.

## setup

    make setup

This runs `opam init --bare --no-setup`, creates the switch in this directory and installs dune, yojson and alcotest. Creating the switch compiles OCaml and takes several minutes. Every OCaml command goes through `opam exec --`.

## build and test

    make build    # dune build, copies the moth executable to bin/moth
    make test     # native C++ unit tests (build/native), then dune test
    make helper   # the Windows helper (x86_64 only) and the stand-in game (64-bit and 32-bit), into bin/
    make clean

Variables: `WINE` (path to CrossOver's wine) and `MOTH_TEST_BOTTLE` (default `uhmm`), both overridable on the command line.

Build output (`_build/`, `_opam/`, `build/`, `bin/`) is gitignored.

## memory budget

`MOTH_BUDGET_MB` is read once, when the daemon starts. A whole number from 1 to 65536 is the most memory, in MiB, the helper may use to hold a hunt's candidates; every `find` sends it with its scan. Past it, the helper stops tracking groups from the end of the hunt's list (the `untracked` note) and searches them again on the next number. Unset or empty, the helper's own budget holds (1536 MiB). Anything else is ignored, with a line in `core.log`. A running daemon keeps the value it started with: `moth stop` first, then run the next command with the new value.

`MOTH_SNAPSHOT_MB` is read the same way, once, when the daemon starts: a whole number from 1 to 1048576 is the most memory, in MiB, that a `snapshot` may take. The default is 8192. A snapshot costs as much memory as the game's writable memory, held by the helper until the hunt has narrowed: it is released once every group is at or under 64,000,000 places and its values fit the budget. While a snapshot hunt narrows, the helper can briefly hold about the snapshot plus twice the budget. A snapshot hunt follows aligned values in six forms: 4-byte and 8-byte whole numbers and decimals, 2-byte whole numbers, and 1-byte whole numbers in the wider tier. A value at an odd address is found by typing its number, not by how it moves. Above about 5.3 GiB of writable memory, the 1-byte form is not followed by change at the default budget. Apart from snapshots, a scan with very many matches (a dense scan) can briefly run about 2 to 3.5 GiB over the budget while it stores their values. Anything else in `MOTH_SNAPSHOT_MB` is ignored, with a line in `core.log`.

## kept values

`moth kept --json` gives each kept value's `set_to` and `put_back`. `set_to` is what the value's newest write wrote, shown as the value shows numbers, or null when there is none. Its newest write is the newest that `undo` can still reverse at its address and as its value type, made by row or by name, before or after `keep`. `put_back` is true when the value reads as it did before that write: the bytes there now are the ones the write replaced, not the ones it wrote (never when it can't be read). The text form says `you set it to <set_to>; it is back to what it was before.` after the value's line. Neither says why; the game working the value out again and ordinary play look the same.

## the feed, and reads that leave no trace

A client can follow what moth does, whoever asks it. `watch` (opts: `after`, an event number) is answered `{"id":..,"ok":true,"result":{"seq":N},"text":""}`, N being the newest recorded event, and the connection then stays open. Each recorded request sends `{"busy":{"actor":..,"cmd":..,"args":[..]}}` as it starts and `{"event":{..}}` once it ends, the object being the line `events.jsonl` gained. With `after` = k, the stored events after k come first as event lines, oldest first, the newest 1,000 at most. Sixteen watchers at most; the next is answered `too many watchers`. The daemon never waits for a watcher: one whose socket can't take a line at once is closed, and should watch again with `after` set to the newest `seq` it has. A last line without a newline was cut off there; ignore it. The connection also closes when the daemon stops, after the stop's own event. `moth watch [--after <seq>] [--json]` prints the feed, an event as `moth log` does and a start as `working: <cmd> <args> (<actor>)`, watches again by itself if it is dropped, and ends with `moth stopped`.

A request with `"record": false` is answered as usual but takes no event number, adds no line to `events.jsonl` and tells no watcher: for polling. Only `bottles`, `ps`, `status`, `results`, `kept`, `log` and `games` may ask; any other command is refused, unrecorded, with `only commands that change nothing can go unrecorded`. One exception: a `status` that finds the game closed is recorded anyway, and watchers get its event with no `busy` line before it. The command line never sends `record`; `watch` is never recorded.

## engine speed

    make bench-native   # builds build/native/moth_bench and runs it

It prints four lines over a seeded 256 MiB fake memory. Each figure is the best of three rounds, and GB means 2^30 bytes.

- `scan: <x> s/GB, narrow: <y> ms`: a first scan and one narrow, using the twelve groups a search for the whole number 100 makes.
- `dense narrow: <a> s/GB on 1 worker, <b> s/GB on <n> workers`: a `same` step over every aligned 4-byte place, which keeps them all and stores their values again.
- `snapshot: <c> s/GB, first same step: <d> s/GB on <n> workers`: a snapshot with the six groups a snapshot hunt carries, then a first `same` step over them.
- `51M u8 values gained: <e> s/GB from a snapshot, <f> s/GB from a scan`: a step that leaves a 1-byte group with about 51 million places, which then take their values, once after a snapshot and once after a scan.

`build/native/moth_bench <workers>` tries another worker count for the first line. The default is 0 (auto), which is what the helper uses.

The figures are only a relative guide. Here, reading memory is a memcpy, so it measures the engine's own comparison work on Apple Silicon. The real helper is x86_64 code running under Rosetta inside wine. It reads memory through wine on one thread (`make bench`, below, measures the whole of it). Scanning reads on one thread and compares on the workers, so comparisons overlap the reads.

Measured on 2026-11-11 on an Apple M5 (10 cores), where auto means 8 workers:

| engine | scan | narrow |
| --- | --- | --- |
| auto (8 workers) | 0.28 to 0.30 s/GB | 13 ms |
| 1 worker | 1.54 s/GB | 13 ms |
| before parallel scanning (task 4) | 3.41 s/GB | 19.6 ms |

For a closer look at the real case, the same program was cross-compiled once with mingw-w64 (x86_64, `-O3 -static`) and run through CrossOver's wine, so under Rosetta. It gave 0.48 s/GB with auto workers, 2.37 s/GB with one, and a 34 ms narrow. There is no make target for this.

## end to end in a bottle

    make test-integration   # every scenario, one line each; exits 1 if any fails
    make bench              # scenario 10 alone: the real helper's scan speed

Both build everything first, then run `core/integration/it_main.exe` (its own dune executable, not part of `dune test`). It launches the stand-in game (`bin/moth-target.exe`, or `bin/moth-target32.exe`) in the bottle `MOTH_TEST_BOTTLE` (default `uhmm`, a scratch bottle) with `WINE`, and drives `bin/moth ... --json` against it, checking each reply with what the game reports over its control port. Each scenario gets a fresh game. Everything runs under a new `MOTH_HOME` in `/tmp`; afterwards the daemon is stopped (which ends the helper) and the games are closed, whether the scenarios passed or not. The home is removed when everything passed and kept for its logs (`core.log`, `helper.log`, `target.log`) when something failed. `it_main.exe [--bin <dir>] [-v] [scenario numbers]` runs a few scenarios by hand; `-v` shows each moth command and what it printed.

Scenario 12 (anti-cheat names) is not run in a bottle: it prints `skip`, and the daemon's unit tests cover it with a fake helper.

Measured on 2026-11-11 on an Apple M5 (10 cores) with CrossOver 26.3, a 64-bit target with a 1 GiB random filler, over fifteen runs:

| step | time |
| --- | --- |
| `find 100` (scan, about 1.00 GB read) | 0.60 to 0.87 s/GB, the budget is 3.0 |
| `next 100` (narrow, the same groups) | 450 to 590 ms in the helper, about 4 ms more end to end |

The whole suite, scenarios 1 to 30, takes about 30 seconds once the bottle is up (30 and 31 seconds over two runs on 2026-11-12).
