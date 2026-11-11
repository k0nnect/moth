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
    make helper   # Windows helper, 64-bit and 32-bit, into build/win64 and build/win32 (bin/ later)
    make clean

Variables: `WINE` (path to CrossOver's wine) and `MOTH_TEST_BOTTLE` (default `uhmm`), both overridable on the command line.

Build output (`_build/`, `_opam/`, `build/`, `bin/`) is gitignored.

## engine speed

    make bench-native   # builds build/native/moth_bench and runs it

It prints `scan: <x> s/GB, narrow: <y> ms` for a first scan and one narrow over a seeded 256 MiB fake memory, using the twelve groups a search for the whole number 100 makes. Each figure is the best of three rounds, and GB means 2^30 bytes. `build/native/moth_bench <workers>` tries another worker count. The default is 0 (auto), which is what the helper uses.

The figure is only a relative guide. Here, reading memory is a memcpy, so it measures the engine's own comparison work on Apple Silicon. The real helper is x86_64 code running under Rosetta inside wine. It reads memory through wine on one thread (`make bench`, below, measures the whole of it). Scanning reads on one thread and compares on the workers, so comparisons overlap the reads.

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

The whole suite takes about 10 seconds once the bottle is up.
