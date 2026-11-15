# moth

A memory scanner and editor for single-player games, built around a coding agent: the job Cheat Engine does, for Windows games running under CrossOver on an Apple Silicon Mac. Written in OCaml and C++.

You type the number you see on screen. moth searches every way that number could be stored, so you never pick a data type. The window has the hunt on one side and a real terminal on the other, with Claude Code running in it on the same session: it sees every scan you click, says what to do next, and can read and write the game's values itself.

The name: the first computer "bug" was a moth taped into the Harvard Mark II logbook in 1947.

## status

**Unfinished. Work stopped on 2026-11-15 and the repository is published as it stood.** Three of the five planned stages were never built.

| part | state |
| --- | --- |
| the engine and the `moth` command (find, narrow, keep, set, undo, hunts with no number) | built, tested, and tried by hand on Disco Elysium ([what was seen](docs/verification/disco-elysium.md)) |
| the window (`moth-app`) | built, tested, and tried by hand on Disco Elysium |
| the terminal in the window, and the agent on the session (`moth agent`) | built and tested, and run with the real Claude Code against a stand-in game. Never tried on a real game |
| structure discovery (what a value is, paths that survive a restart) | not built |
| trainers (toggles that hold a value) | not built |
| the overlay over the game | not built |

It has been run on one machine and one game. Expect rough edges anywhere else. The known limits of the last stage are listed at the end of [its plan](docs/superpowers/plans/2026-11-13-moth-stage-2b-terminal-and-agent.md).

## what it is for, and what it is not

moth is for single-player, offline games you own. It does not get around anti-cheat and has nothing for online games. It injects nothing into the game: a small helper program runs beside the game in the same CrossOver bottle and reads and writes its memory through the ordinary Windows calls. It needs no sudo, leaves System Integrity Protection on, and does not re-sign CrossOver.

## how it is put together

- `helper/`: `moth-helper.exe`, C++20, built with mingw-w64. Runs in the game's bottle and does the scanning.
- `core/`: the background service and the `moth` command, OCaml. The service owns the session and keeps a log of every command, from anyone.
- `app/`: `moth-app`, a Qt 6 Widgets window with a terminal built on libvterm. It starts `moth agent` in that terminal.
- `moth agent` starts Claude Code with a settings file of its own for that session (one permission, `moth` commands, and a hook that tells the agent what you did). It writes nothing under `~/.claude`.

## build

Needs macOS on Apple Silicon, CrossOver, and from Homebrew: `opam`, `cmake`, `mingw-w64`, `qtbase`, `libvterm`, `pkgconf`. Claude Code is needed only for the agent.

    make setup    # a local opam switch with OCaml 5.5; several minutes, touches no shell profile
    make build    # bin/moth
    make helper   # bin/moth-helper.exe
    make app      # bin/moth-app
    make test

[docs/dev.md](docs/dev.md) has the rest, including the end-to-end tests that run in a CrossOver bottle.

## use

With a game running in a bottle:

    bin/moth bottles
    bin/moth attach --bottle "Disco Elysium" disco.exe
    bin/moth find 32          # the number the game shows
    # change the number in the game, then
    bin/moth next 42
    bin/moth results
    bin/moth keep 1 xp
    bin/moth set xp 90
    bin/moth undo

`bin/moth help` lists every command. `bin/moth-app` opens the window, which does the same by clicking and starts the agent in its terminal.

## documents

- [the design](docs/superpowers/specs/2026-11-11-moth-design.md)
- [the plans, one per stage](docs/superpowers/plans/)
- [what was seen on a real game](docs/verification/disco-elysium.md)

## licence

MIT, see [LICENSE](LICENSE). The two vendored headers in `third_party/` are MIT too. Qt and libvterm are linked from Homebrew and are not part of this repository.
