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

The figure is only a relative guide. Here, reading memory is a memcpy, so it measures the engine's own comparison work on Apple Silicon. The real helper is x86_64 code running under Rosetta inside wine. It reads memory through wine on one thread, and that reading takes most of the 3 s/GB budget. Scanning reads on one thread and compares on the workers, so comparisons overlap the reads.

Measured on 2026-11-11 on an Apple M5 (10 cores), where auto means 8 workers:

| engine | scan | narrow |
| --- | --- | --- |
| auto (8 workers) | 0.28 to 0.30 s/GB | 13 ms |
| 1 worker | 1.54 s/GB | 13 ms |
| before parallel scanning (task 4) | 3.41 s/GB | 19.6 ms |

For a closer look at the real case, the same program was cross-compiled once with mingw-w64 (x86_64, `-O3 -static`) and run through CrossOver's wine, so under Rosetta. It gave 0.48 s/GB with auto workers, 2.37 s/GB with one, and a 34 ms narrow. There is no make target for this.
