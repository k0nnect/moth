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
