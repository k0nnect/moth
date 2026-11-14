# moth stage 2b: the terminal and the agent — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** moth's window opens with Claude Code already running in a real terminal beside the panels; the agent is told what the person clicks, speaks up with the next move, and acts on the same hunt through the `moth` command with no permission prompt.

**Architecture:** The agent is one more client of the core, driving it with the `moth` command under the actor `agent`. `moth agent` writes a per-session settings file and execs Claude Code with it; nothing of the user's own agent setup is edited. The person's actions reach the agent through a `Stop` hook with `asyncRewake` running `moth wait`, which follows the core's feed, bundles what the person did and wakes the idle session with a note. The terminal is libvterm's screen painted by a Qt widget over a pty, in the splitter beside the column stage 2a built. All of the agent hookup works from any terminal; moth's own terminal only hosts it.

**Tech Stack:** OCaml 5.5.2 with dune, yojson, alcotest; C++20, CMake, Qt 6.12 `qtbase` (Core, Gui, Widgets, Network, Test), libvterm 0.3.3 from Homebrew (MIT), `forkpty`; Claude Code 2.1.323 or later for the proof.

**Spec:** `docs/superpowers/specs/2026-11-11-moth-design.md` (sections 6.1, 6.2, 6.3, 8 and 9, stage 2). Also read `docs/notes/2026-11-12-app-toolkit-test.md` (what was tried, and what was left untried, for the terminal and for waking the agent) and the "Amendments" section of `docs/superpowers/plans/2026-11-12-moth-stage-2a-window.md` (the protocol and the window as built).

## Global Constraints

- moth's own source is OCaml and C++. No Objective-C++ in this plan.
- No dependency under the GPL. libvterm is the one new library. Qt stays `qtbase` only, linked dynamically.
- Nothing of the user's own agent setup is edited: moth never writes under `~/.claude` or to `~/.claude.json`. Everything the agent needs is in files under `$MOTH_HOME/agent/`, passed for that session only.
- moth never answers a Claude Code prompt for the user (folder trust, login, permission): it types nothing into the agent by itself.
- Every panel control works with no agent running, with Claude Code not installed, and with the terminal showing a plain shell.
- The agent acts through the `moth` command and can do nothing the command cannot. Only `moth` is approved for it in advance; everything else follows the user's own settings.
- The instructions moth gives the agent say single-player and offline only, and tell it to decline online use and anti-cheat workarounds.
- The user never chooses a data type; the agent is told never to ask for one.
- No silent failure: a missing agent, a service from another build, a refused command are each said in plain words where the person is looking.
- Writing to the wrong place is the worst failure. An answer to a row write names the address it wrote to.
- The app talks to the core's socket only, and never blocks its interface on a request or on the pty.
- Text in the window is lowercase plain words in the command's vocabulary.
- Automated tests never run the real `claude` (a stub stands in through `MOTH_CLAUDE`), use only the `uhmm` bottle and a temporary `MOTH_HOME` (at least one test per task that touches paths uses a home with a space in it), run windows with `QT_QPA_PLATFORM=offscreen`, and leave no `moth daemon`, `moth-helper`, `moth-target`, `moth-app`, `moth wait`, shell or stub process behind.
- SIP stays on, no `sudo`, CrossOver is not re-signed, nothing is injected into a game.
- Commits: lowercase message, no attribution or co-author trailer. Local git only; no remote is added and nothing is pushed.
- If a test's expectation has to change because of this plan (the row-write texts in Task 1 do this), change it in the task that causes it and list every one in the report.

## Review Focus

1. **A home with a space in it.** The default is `~/Library/Application Support/moth`. The hook's command line, the settings file's paths and the terminal's start command must all survive it.
2. **Two waiters.** The person types to the agent while a waiter is armed; the turn ends and a second waiter starts. The next click brings one note, not two.
3. **The session ends any way it can** (`/exit`, the window closed, the agent killed): no `moth wait`, shell or agent process is left.
4. **A flood in the terminal** (`yes`, a megabyte of output): the window stays responsive, memory stays bounded, and processor use returns to idle afterwards.
5. **Claude Code missing or not logged in:** the terminal says so in plain words and leaves a working shell, and every panel still works.

Each has a test in the task that owns the code (Tasks 3, 2, 2 and 8, 7, 3 and 8).

## Protocols

Additions to the client protocol and the command line. Everything not listed is as stage 2a left it.

| item | shape | rule |
| --- | --- | --- |
| env `MOTH_ACTOR` | `agent`, `user`, unset or empty | read by the `moth` command for every request it sends: `agent` sends actor `agent`; `user`, unset or empty send `user`; anything else exits 2 with `MOTH_ACTOR must be user or agent`. |
| event field `act` | boolean, in every event | true when the command is not one of the reads (`bottles`, `ps`, `status`, `results`, `kept`, `log`, `games`), or when the daemon let go of the game or the helper during it. False otherwise. |
| `status --json` | adds `"proto": 2` and `"quiet": false` | in all four shapes of the answer. `Daemon.protocol = 2`. A service that answers no `proto` is counted as 1. |
| answer to a row write | `set #1 (0x6c55dfb8) to 98 (was 97)`, `kept #2 (0x6c55dfb8) as xp (42, whole number)` | the row's address in brackets after the row number, in the text and so in the event's summary. Writes and keeps by name or by address keep their texts. |
| command `quiet` | `quiet on`, `quiet off`, `quiet` | recorded. `on`: `quiet: the agent is not told what you do. it can still look: moth log`. `off`: `the agent is told what you do again`. No argument: `quiet is on` or `quiet is off`. Anything else: `quiet takes on or off`. Kept in the daemon and in `$MOTH_HOME/quiet` (present means on), so it outlives a restart of the service. |
| `Client.watch` | gains `?started:(int -> unit)` and `?idle:(unit -> bool)` | `started` is called once with the `seq` in the reply. `idle` is called whenever half a second passes with no line; returning false ends the watch with `Ok ()`. |
| command `wait` | `moth wait --session <dir>` | not sent to the daemon as a request. See Task 2. Exit 2 with notes on stderr, or exit 0 silently. |
| command `agent` | `moth agent` | not a request. See Task 3. |
| env `MOTH_CLAUDE` | a program | what `moth agent` runs instead of looking for `claude` on `PATH`. For tests. |
| env `MOTH_TERMINAL` | a shell command line | what the window's terminal runs first. Unset: `moth agent`. Empty: nothing, a plain shell. |

Help lines, after `watch`:

```
  quiet [on|off]                       stop or resume telling the agent what you do
  agent                                start claude code on this moth session
```

`wait` is not listed in `moth help`; `moth wait` with no `--session` answers `wait is run by the agent's hook: moth wait --session <dir>`.

---

### Task 1: Who did it, and whether it changed anything

**Files:**
- Modify: `core/lib/daemon.ml`, `core/lib/daemon.mli`, `core/lib/session.ml`, `core/lib/client.ml`, `core/lib/client.mli`, `core/bin/main.ml`, `docs/dev.md`
- Test: `core/test/test_daemon.ml`, `core/test/test_session.ml`, `core/test/test_client.ml` (create it if actor parsing has no home yet), `core/integration/it_main.ml` (expectations only)

**Interfaces:**
- Produces: `MOTH_ACTOR`, the `act` field, `proto` in `status --json`, the row-write texts, exactly as in Protocols.
- Produces: `Daemon.protocol : int` (2).
- Produces: `Client.actor_of_env : string option -> (string, string) result`, given the value of `MOTH_ACTOR`.
- Produces: `Client.watch : home:string -> ?after:int -> ?started:(int -> unit) -> ?idle:(unit -> bool) -> (Yojson.Safe.t -> bool) -> (unit, string) result`.
- Changes: `moth watch`, dropped before it has printed an event, watches again from the `seq` its first reply gave (it skipped events before).

- [ ] **Step 1: Write the failing tests**

One test each:
- `Client.actor_of_env`: `None`, `Some ""` and `Some "user"` give `Ok "user"`; `Some "agent"` gives `Ok "agent"`; `Some "Agent"` and `Some "bot"` give `Error "MOTH_ACTOR must be user or agent"`;
- a request sent with actor `agent` is recorded with `"actor":"agent"`, and its `busy` line carries it;
- `find 3`, `set`, `keep`, `forget`, `undo`, `attach`, `detach` events have `"act":true`; recorded `status`, `results`, `kept`, `log`, `bottles`, `games` events have `"act":false`;
- an unrecorded `status` that finds the game closed is recorded with `"act":true`;
- `status --json` has `"proto":2` when not attached, attached with no hunt, attached with a hunt, and just closed;
- `set 3 9` answers `set #3 (<row 3's address>) to 9 (was <old>)` and `keep 3 hp` answers `kept #3 (<address>) as hp (<value>, <tag>)`; `set hp 9` and `keep 0x10 5 hp` answer as before;
- `Client.watch` calls `started` with the reply's `seq` before any line, and calls `idle` at least once in a second with no traffic; `idle` returning false ends it with `Ok ()`.

- [ ] **Step 2: Run them and see them fail**

Run: `opam exec -- dune test`
Expected: FAIL (`Unbound value Client.actor_of_env`).

- [ ] **Step 3: Implement**

`main.ml` reads `MOTH_ACTOR` once, before parsing, and fails with exit 2 on a bad value. The watch loop in `main.ml` keeps the `started` seq as its `newest` until an event replaces it.

- [ ] **Step 4: Update every expectation the new texts break**

Unit tests and `it_main.ml` scenarios that match `set #` or `kept #`. List each in the report.

- [ ] **Step 5: Run every unit test and the bottle suite**

Run: `make test && make helper && make test-integration`
Expected: all pass (scenario 12 is the documented skip).

- [ ] **Step 6: Commit**

```bash
git add core docs/dev.md && git commit -m "moth: an agent's commands carry its name, events say whether they acted, a row write says where it wrote"
```

### Task 2: Quiet, and waiting for the person

**Files:**
- Create: `core/lib/wait.ml`, `core/lib/wait.mli`
- Modify: `core/lib/daemon.ml`, `core/lib/client.ml`, `core/lib/client.mli`, `core/bin/main.ml`, `core/lib/dune` if modules are listed, `docs/dev.md`
- Test: `core/test/test_wait.ml` (new), `core/test/test_daemon.ml`

**Interfaces:**
- Consumes: `act`, `Client.watch` with `started` and `idle` (Task 1).
- Produces: the `quiet` command and `"quiet"` in `status --json`, as in Protocols.
- Produces `Wait`, the bundling rule with no sockets or clock of its own:

```ocaml
type t
val start : cursor:int -> quiet:bool -> t
(** a feed line (a busy line or an event), seen at [now] *)
val line : t -> now:float -> Yojson.Safe.t -> unit
type verdict = Keep_waiting | Tell of { notes : string; cursor : int }
(** called after every line and every idle tick *)
val check : t -> now:float -> verdict
(** the newest seq taken in, told or not *)
val cursor : t -> int
```

- Produces: `moth wait --session <dir>`.

The rule `Wait` implements:
- An event counts when its `seq` is above the cursor, its `actor` is not `agent`, its `act` is true and its `cmd` is not `quiet`.
- A `quiet` event (from anyone) sets the quiet state from its first argument when it succeeded. While quiet, counting events are dropped as they arrive. Turning quiet off replays nothing.
- `Tell` when: at least one event is held, no `busy` line is waiting for its event, and 1.5 seconds have passed since the last line, or 10 seconds since the first held event, whichever comes first.
- `notes` is `[moth] the person did this in moth:` and then one line per held event, oldest first: `- <cmd and args>: <summary>`, or `- <cmd and args>: failed: <summary>` when `ok` is false. More than 12 held: the first line after the heading is `- (<n> earlier steps: moth log)` and the newest 12 follow.
- `cursor` in `Tell` is the newest seq taken in.

What `moth wait --session <dir>` does around it:
- Reads nothing from stdin (the hook is handed JSON there; it must not block on it).
- `<dir>`'s last path component is the process id of the agent session. When that process is not alive, or `<dir>` is gone, it exits 0. It checks this at its start and on every idle tick.
- Writes its own pid to `<dir>/waiter`. On every idle tick and just before telling, it reads `<dir>/waiter` again: another pid there means a newer waiter has taken over, and it exits 0 without telling.
- Cursor: the number in `<dir>/cursor`, or, with no such file, the `seq` the watch reply gave. Quiet at its start: `status --json` read unrecorded.
- A service that is not running, or drops the feed: it tries again every second, from its cursor, for as long as the session lives. A service whose `proto` is not `Daemon.protocol`: exits 0.
- On `Tell`: writes the cursor to `<dir>/cursor`, prints the notes to stderr, exits 2. It writes `<dir>/cursor` whenever the cursor moves while quiet, too.

- [ ] **Step 1: Write the failing tests**

`test_wait.ml`, with made-up feed lines and times:
- a user `find 100` event at t=0 and nothing more: `Keep_waiting` at t=1.4, `Tell` at t=1.6 whose notes are the heading and `- find 100: <summary>` and whose cursor is that seq;
- three user events half a second apart are told together, once, 1.5 seconds after the third;
- a `busy` line after the first event holds the telling until its event has come and 1.5 seconds have passed;
- events arriving every second without pause are told at t=10 after the first;
- an `agent` event, a user `status` (`act` false) and a user `quiet on` are never told and still move the cursor;
- after `quiet on`, a user `find` is not told; after `quiet off`, a later user `next` is told alone;
- a failed `next 5` is told as `- next 5: failed: <summary>`;
- 15 held events are told as `- (3 earlier steps: moth log)` and 12 lines;
- an event at or below the starting cursor is ignored.

`test_daemon.ml`:
- `quiet on` then `status --json` has `"quiet":true`; a daemon started again on the same home still has it; `quiet off` removes `$MOTH_HOME/quiet`; `quiet maybe` answers `quiet takes on or off`;
- `moth wait` against a running test daemon in a home with a space in its path: started with a session dir named for the test's own pid, then a user `find 3`, it exits 2 within 4 seconds with stderr starting `[moth] the person did this in moth:` and `<dir>/cursor` holding that event's seq; started again, it is still running after 3 seconds;
- two waiters on one session dir, the second started a second after the first: after a user `find 3`, exactly one exits 2 and the other exits 0;
- a session dir named for a pid that is not alive: exits 0 within 2 seconds;
- with no daemon running it is still waiting after 3 seconds, and exits 2 once a daemon is started and a user `find 3` is sent.

- [ ] **Step 2: Run them and see them fail**

Run: `opam exec -- dune test`
Expected: FAIL (`Unbound module Wait`).

- [ ] **Step 3: Implement `Wait`, `quiet`, and `moth wait` in `main.ml`**

- [ ] **Step 4: Run every unit test**

Run: `make test && bin/moth help`
Expected: all pass; the help lists `quiet` and does not list `wait`.

- [ ] **Step 5: Commit**

```bash
git add core docs/dev.md && git commit -m "moth: quiet, and wait: the person's actions bundled into a note for the agent"
```

### Task 3: `moth agent`

**Files:**
- Create: `core/lib/agent.ml`, `core/lib/agent.mli`
- Modify: `core/bin/main.ml`, `docs/dev.md`
- Test: `core/test/test_agent.ml` (new)

**Interfaces:**
- Consumes: `moth wait` (Task 2), `Daemon.protocol` (Task 1).
- Produces:

```ocaml
(** what the agent is told about moth, appended to its own system prompt *)
val instructions : string
(** what moth says to it first, so its first turn ends and the hook is armed *)
val first_prompt : string
(** the settings for one session: env, the one approval, the Stop hook *)
val settings : home:string -> moth:string -> path:string -> dir:string -> Yojson.Safe.t
(** makes [home]/agent/<pid> (0700) with settings.json and instructions.md, makes
    [home]/agent/work, and removes sibling session folders whose process is not alive. Returns
    the session folder. *)
val prepare : home:string -> moth:string -> path:string -> pid:int -> (string, string) result
(** the program and arguments to run *)
val command : claude:string -> dir:string -> string array
```

- Produces: `moth agent`.

`settings` is exactly this object, with `<moth>` the absolute path of the running `moth`, `<mothdir>` its folder, `<path>` the `PATH` it was started with, and both paths in the hook's command wrapped in single quotes (a single quote inside a path written as `'\''`):

```json
{
  "env": { "MOTH_ACTOR": "agent", "MOTH_HOME": "<home>", "PATH": "<mothdir>:<path>" },
  "permissions": { "allow": ["Bash(moth *)"] },
  "hooks": { "Stop": [ { "hooks": [ { "type": "command", "command": "'<moth>' wait --session '<dir>'", "asyncRewake": true, "timeout": 86400 } ] } ] }
}
```

`command` is `[| claude; "--settings"; <dir>/settings.json; "--append-system-prompt"; instructions; first_prompt |]`.

`first_prompt`: `moth just opened. run moth status, then tell me in a line or two where things stand and what to do next.`

`instructions` (this exact text, also written to `instructions.md` for the person to read):

```
you are the agent built into moth, a memory scanner and editor for single-player games. the person is playing a game and hunting for values in it (health, money, a stat) so they can change them. you and they share one hunt: what they click in moth's window and what you run both act on it, and both show in the window.

how you work
- you drive moth with the moth command in the shell. moth help lists every command. moth status says what is attached and where the hunt stands. moth log lists what has been done, by them and by you.
- moth commands are approved in advance. run them without asking, including ones that write to the game (moth set). every write can be put back with moth undo. say what you did in a line.
- a note that starts with [moth] is what the person just did in the window. they did not type it. answer with the next move in one or two short lines: what to do in the game, and which button or command comes after. if the next step is yours, do it.
- the person never chooses a data type and you never ask for one. moth searches every way a number can be stored.
- to find a value: moth find <the number on screen>, have them change it in the game, moth next <the new number>, until few are left. with no number to read: moth snapshot, then moth next down, up, same or changed. moth results lists what is left, moth keep <#n> <name> keeps one, moth set <name> <number> changes it.
- when you write to a row by its number, add --at with the address moth results showed for it, so the write cannot land on another row.
- be brief. no headings. they are in the middle of a game.

limits
- single-player and offline play only. if they ask for help in an online or multiplayer game, or with getting around anti-cheat, say no and say why. moth refuses to attach when it sees a known anti-cheat. do not look for a way around that.
- use nothing but moth on the game: no other tool on its process, nothing loaded into it.
- when moth says something failed, tell them in moth's words. never assume a step worked.
```

What `moth agent` does: resolves the program (`MOTH_CLAUDE`, else `claude` found on `PATH`); checks the service's `proto` with `status --json` (this starts the service, as any command does); `prepare` with its own pid; changes to `<home>/agent/work`; sets `MOTH_ACTOR`, `MOTH_HOME` and `PATH` in its own environment as in the settings; execs `command`. Its texts:
- no program: `claude code is not installed, or not on PATH. moth works without it: use the window, or type moth commands here.` and exit 1;
- a service from another build: `moth's background service is from another build of moth. stop it and it starts again by itself: moth stop` and exit 1;
- `moth agent` with any argument: `agent takes no arguments`, exit 2.

- [ ] **Step 1: Write the failing tests**

`test_agent.ml`, in a temporary home whose path has a space and a single quote in it:
- `settings` equals the object above for given `home`, `moth`, `path`, `dir`;
- `sh -c <the hook's command>` with `moth` replaced by a script that prints its arguments one per line prints `wait`, `--session` and the folder, whole;
- `prepare` makes `agent/<pid>/settings.json` (parses, equals `settings`), `agent/<pid>/instructions.md` (equals `instructions`), `agent/work`, all mode 0700 or 0600; a sibling folder named for a dead pid is removed and one named for a live pid (the test's own) is kept;
- `instructions` contains `single-player and offline play only`, `never ask for one` and `--at`;
- `moth agent` with `MOTH_CLAUDE` set to a stub script that writes its arguments, its working directory and `$MOTH_ACTOR`, `$MOTH_HOME`, `$PATH` to a file: the arguments are `command`'s, the directory is `<home>/agent/work`, `MOTH_ACTOR` is `agent`, and `PATH` starts with moth's folder; the stub's pid is the pid in the session folder's name;
- `MOTH_CLAUDE=/nonexistent moth agent` and an empty `PATH` both print the not-installed text and exit 1, and no `agent/<pid>` folder is left;
- nothing under the test's `HOME` (set to a temporary folder) other than the moth home was created or changed.

A test that lets `moth agent` reach the service (the stub one) stops that service when it is done.

- [ ] **Step 2: Run them and see them fail**

Run: `opam exec -- dune test`
Expected: FAIL (`Unbound module Agent`).

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run every unit test**

Run: `make test && bin/moth help`
Expected: all pass; the help lists `agent`.

- [ ] **Step 5: Commit**

```bash
git add core docs/dev.md && git commit -m "moth: agent, claude code started on the session with its instructions, its one approval and its hook"
```

### Task 4: The window says less, and says it right

The rough edges the stage 2a proof found (`docs/verification/disco-elysium.md`, stage 2a, problems 2 to 6), and the service from another build.

**Files:**
- Modify: `app/src/board.hpp`, `app/src/board.cpp`, `app/src/search_bar.cpp`, `app/src/results_panel.cpp`, `app/src/kept_panel.cpp`, `app/test/fake_core.hpp`, `app/test/fake_core.cpp`
- Test: `app/test/test_board.cpp`, `app/test/test_search_bar.cpp`, `app/test/test_results.cpp`, `app/test/test_kept_panel.cpp`

**Interfaces:**
- Consumes: `act`, `proto`, `quiet` in `status --json` (Tasks 1 and 2).
- Produces on `Board`: `bool quiet() const` (from the last status read; `changed` fires when it flips) and `bool stale() const` (the last status read had a `proto` other than 2, or none).
- `FakeCore`: status answers carry `"proto":2` and `"quiet":false` unless a test sets otherwise; pushed events carry `act`.

Behaviour:
- An event whose `act` is false never replaces the window's line. One whose `act` is true does, as now (`agent: ` before an agent's summary). An event with no `act` field counts as true.
- While `stale()`: the line is `moth's background service is from another build of moth. stop it and it starts again by itself: moth stop`, `Board::run` refuses with that line and sends nothing, and polls go on (so the line clears by itself once the service is restarted).
- The results panel's label `shown` reads `showing <loaded> of <total>` only while fewer rows are loaded than exist, and is empty otherwise. The count and its breakdown stay in the search bar.
- The results table's `kind` column is as wide as `whole number (8-byte)` in the table's font plus its padding; `#` fits six digits; `address` takes the rest.
- `undo last change` is disabled while no game is attached.
- After a step that worked (`find`, `next <number>`, `snapshot`, `next down|up|same|changed`) the number box is emptied and keeps the focus. After one that failed it keeps what was typed.

- [ ] **Step 1: Write the failing tests**

One test each:
- a pushed `log` event with `"act":false` and a long summary leaves the line as it was; a pushed `set` event with `"act":true` and actor `agent` makes the line `agent: <summary>`;
- a status answer with `"proto":1`, and one with no `proto`, make `stale()` true and the line the sentence above; `run("find", {"3"})` then returns false and `FakeCore::seen()` gains no `find`; a later status with `"proto":2` clears it;
- a status answer with `"quiet":true` makes `quiet()` true and emits `changed` once;
- with 48 rows all loaded `shown` is empty; with 200 of 105,361 it reads `showing 200 of 105,361`;
- the `kind` column's width is at least `fontMetrics().horizontalAdvance("whole number (8-byte)")`;
- with no game attached the `undo` button is disabled; attached, enabled;
- typing 42 and Return with a hunt on sends one `next 42`; when it answers ok the box is empty, and a second Return sends nothing; when it answers an error the box still reads 42.

- [ ] **Step 2: Run them and see them fail**

Run: `make test-app`
Expected: FAIL.

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run the app's tests and the bottle scenarios that drive the window**

Run: `make test-app && make helper && make test-integration`
Expected: all pass. If scenario 33 to 36 expectations change (the `shown` label, the row-write text), change them here and say so.

- [ ] **Step 5: Commit**

```bash
git add app core/integration && git commit -m "app: other people's looks leave the line alone, one count, a kind column that fits, and a service from another build is said"
```

### Task 5: A pty and a screen

**Files:**
- Create: `app/src/pty.hpp`, `app/src/pty.cpp`, `app/src/term_screen.hpp`, `app/src/term_screen.cpp`
- Modify: `app/CMakeLists.txt` (libvterm through `pkg_check_modules(VTERM REQUIRED IMPORTED_TARGET vterm)`, the two sources, the two tests), `Makefile` only if the configure line needs it, `docs/dev.md` (setup: `brew install libvterm`)
- Test: `app/test/test_pty.cpp`, `app/test/test_term_screen.cpp`

**Interfaces:**

```cpp
// a child process on a pseudo-terminal. nothing here blocks except the destructor, briefly.
class Pty : public QObject {
  Q_OBJECT
public:
  explicit Pty(QObject *parent = nullptr);
  // SIGHUP to the child's process group; SIGKILL to it if it is still there after 500 ms
  ~Pty() override;
  // env is the whole environment of the child. false, with nothing started, when the program
  // cannot be run
  bool start(const QString &program, const QStringList &args, const QStringList &env,
             const QString &cwd, int rows, int cols);
  void write(const QByteArray &bytes);
  void resize(int rows, int cols);
  bool running() const;
  qint64 pid() const;
signals:
  void output(const QByteArray &bytes);
  void exited(int status);   // the exit code, or 128 + the signal
};

struct TermCell {
  QString text;              // "" for an empty cell and for the second half of a wide one
  QColor fg, bg;             // invalid QColor: the default
  bool bold, italic, underline, reverse;
  int width;                 // 1, 2, or 0 for the second half of a wide character
};

// what the terminal shows: libvterm's screen, its scrollback, and what the keyboard and mouse
// send back. no pty and no painting here.
class TermScreen : public QObject {
  Q_OBJECT
public:
  TermScreen(int rows, int cols, QObject *parent = nullptr);
  ~TermScreen() override;
  void feed(const QByteArray &bytes);            // what the program printed
  int rows() const;
  int cols() const;
  void resize(int rows, int cols);
  int scrollback() const;                        // lines scrolled off the top, 10,000 at most
  TermCell cell(int row, int col) const;         // row -1 is the newest scrolled-off line
  QString text(int row) const;                   // a row's text, trailing blanks dropped
  QPoint cursor() const;                         // x is the column, y the row
  bool cursorVisible() const;
  bool altScreen() const;
  bool mouseWanted() const;                      // the program asked for mouse reports
  QString title() const;
  void key(int qtKey, Qt::KeyboardModifiers mods, const QString &text);
  void paste(const QString &text);               // bracketed when the program asked for it
  void mouse(int row, int col, Qt::MouseButton button, bool pressed, Qt::KeyboardModifiers mods);
  void wheel(int row, int col, int steps);       // positive is up; sent only when mouseWanted()
signals:
  void damaged(int firstRow, int lastRow);
  void scrolled();                               // the scrollback grew
  void reply(const QByteArray &bytes);           // what must be written to the program
  void titleChanged();
  void bell();
};
```

- [ ] **Step 1: Write the failing tests**

`test_pty.cpp`:
- `/bin/sh -c 'printf hello; exit 3'`: `output` adds up to `hello` and `exited(3)` comes;
- `/bin/cat`: writing `abc\n` brings `abc` back; destroying the `Pty` leaves no process with that pid within a second;
- a child that ignores SIGHUP (`trap '' HUP; sleep 60`) is gone within a second of the destructor;
- started at 30 by 100, `stty size` prints `30 100`; after `resize(10, 40)` a second `stty size` prints `10 40`;
- `env` reaches the child: `/bin/sh -c 'printf %s "$MOTH_HOME"'` with a value that has a space in it;
- `start("/nonexistent", ...)` returns false and `running()` is false.

`test_term_screen.cpp`:
- `hi\r\n` then `\x1b[31mred`: `text(0)` is `hi`, `cell(1,0)` is `r` with a red `fg`, `cursor()` is (3, 1);
- 24-bit colour, bold, italic, underline and reverse each set their field;
- `日本` takes four columns: widths 2, 0, 2, 0; an emoji with a variation selector is one cell of width 2;
- 100 numbered lines into 24 rows: `scrollback()` is 76, `text(-1)` is line 76's text, `scrolled` fired;
- 20,000 lines: `scrollback()` is 10,000;
- the alternate screen (`\x1b[?1049h`) sets `altScreen()`, adds nothing to the scrollback, and leaving it restores the old rows;
- `\x1b]2;moth\x07` sets `title()` and emits `titleChanged`;
- `key(Qt::Key_Up, ...)` replies `\x1b[A`, and `\x1bOA` after `\x1b[?1h`; Return replies `\r`; Ctrl+C replies `\x03`; `é` replies its two UTF-8 bytes; Option+Left replies `\x1bb`;
- `paste("a\nb")` replies `a\rb`, and `\x1b[200~a\rb\x1b[201~` after `\x1b[?2004h`;
- after `\x1b[?1000h\x1b[?1006h`, `mouseWanted()` is true and a left press at row 2, column 5 replies `\x1b[<0;6;3M`;
- a device status query (`\x1b[6n`) replies the cursor position;
- `resize(10, 40)` changes `rows()` and `cols()` and keeps the text of the bottom rows.

- [ ] **Step 2: Run them and see them fail**

Run: `make test-app`
Expected: FAIL (the files do not exist).

- [ ] **Step 3: Implement**

`Pty` uses `forkpty` and a `QSocketNotifier` on the master, with the master non-blocking and writes queued when it would block. `TermScreen` wraps a `VTerm` with UTF-8 on and its screen layer; scrollback is moth's own ring of rows fed by libvterm's push callback.

- [ ] **Step 4: Run the app's tests**

Run: `make test-app`
Expected: all pass, and `ps` shows no `cat` or `sleep` left by the tests.

- [ ] **Step 5: Commit**

```bash
git add app Makefile docs/dev.md && git commit -m "app: a pty, and a terminal screen over libvterm"
```

### Task 6: The terminal view: drawing and typing

**Files:**
- Create: `app/src/terminal_view.hpp`, `app/src/terminal_view.cpp`
- Modify: `app/CMakeLists.txt`
- Test: `app/test/test_terminal_view.cpp`

**Interfaces:**
- Consumes: `Pty`, `TermScreen` (Task 5).
- Produces:

```cpp
// a terminal: a TermScreen painted, a Pty behind it, the keyboard in front
class TerminalView : public QAbstractScrollArea {
  Q_OBJECT
public:
  explicit TerminalView(QWidget *parent = nullptr);
  TermScreen *screen() const;
  // starts the program at the view's size in cells; false when it cannot be run
  bool run(const QString &program, const QStringList &args, const QStringList &env, const QString &cwd);
  bool running() const;
  qint64 pid() const;
  int paints() const;        // how many times it has painted, for tests
signals:
  void exited(int status);
};
```

Behaviour:
- Font: Menlo 13, Qt's own fallback for what Menlo lacks. A cell is the font's average advance by its line height. The view's size in cells follows the widget's size, never under 2 rows by 10 columns; a change resizes the screen and the pty.
- Painting draws only rows that were damaged, as runs of cells with the same colours and style (not one cell at a time), on the default background `#161616` with default text `#d8d8d8`. Damage is gathered and painted at most once every 16 ms.
- The cursor is a filled block when the view has the focus and an outline when it does not, hidden when the program hides it.
- Every key press goes to `TermScreen::key`; Cmd+C, Cmd+V and Cmd+A are kept for Task 7 and send nothing. Text from an input method (dead keys, the emoji picker, an input method's commit) is sent as text; what is being composed is drawn at the cursor, underlined, and is not sent until committed.
- When the program exits, the last screen stays and `exited` is emitted.

- [ ] **Step 1: Write the failing tests**

Offscreen:
- `run("/bin/sh", {"-c", "printf 'hello\\n'; sleep 5"})`: within 2 seconds `screen()->text(0)` is `hello`, and a grab of the widget has pixels that are not the background in row 0 and only background in row 5;
- `run("/bin/cat")`, then key clicks `a`, `b`, Return: the screen shows `ab` twice (the echo and cat's copy);
- a `QInputMethodEvent` committing `é` to a `cat` shows `é`; a preedit string alone sends nothing to the pty;
- resizing the widget to twice its width doubles `screen()->cols()` (within one), and `stty size` typed into a shell prints the new size;
- `printf` of 5,000 lines: `paints()` rises by fewer than 400 while they arrive;
- with the focus elsewhere, key events sent to another widget reach no pty;
- when the program exits with 3, `exited(3)` is emitted and `text(0)` still holds what it printed.

- [ ] **Step 2: Run them and see them fail**

Run: `make test-app`
Expected: FAIL.

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run the app's tests**

Run: `make test-app`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add app && git commit -m "app: the terminal view, drawn in runs and typed into"
```

### Task 7: The terminal view: looking back, copying, the mouse

**Files:**
- Modify: `app/src/terminal_view.hpp`, `app/src/terminal_view.cpp`
- Test: `app/test/test_terminal_view.cpp`

**Interfaces:**
- Adds to `TerminalView`: `QString selectedText() const;` and `void copy(); void paste();`

Behaviour:
- The vertical scroll bar spans the scrollback. New output keeps the view at the bottom only if it was at the bottom; typing a key returns to the bottom.
- The wheel scrolls the scrollback. On the alternate screen, or when the program asked for mouse reports, the wheel goes to the program instead (`TermScreen::wheel`; on the alternate screen with no mouse reports, as Up and Down keys, three per step).
- When the program has not asked for mouse reports: dragging selects by cells across rows, a double click selects a word, a triple click a row; the selection is drawn reversed and is dropped when the rows it covers change. When it has asked: presses, releases and drags go to the program, and holding Option selects instead.
- Cmd+C copies `selectedText()` (rows joined with newlines, trailing blanks dropped, a row that wrapped joined to the next with nothing between); with nothing selected it does nothing. Cmd+V pastes the clipboard's text through `TermScreen::paste`. Cmd+A selects everything, scrollback included.
- A flood: output is read from the pty in pieces of at most 64 KB per pass of the event loop, so the window keeps answering.

- [ ] **Step 1: Write the failing tests**

- after 200 numbered lines into a 24-row view the scroll bar's maximum is at least 176; wheeling up shows earlier lines (the top row's text changes to a lower number) and new output then leaves the top row where it is; pressing a key returns to the bottom;
- a drag from row 1 column 0 to row 2 column 3 makes `selectedText()` those cells joined by a newline; Cmd+C puts it on `QGuiApplication::clipboard()`;
- a double click on `world` in `hello world` selects `world`;
- a 100-character line in a 40-column view copies as one line with no newline in it;
- with `abc` on the clipboard, Cmd+V into `cat` shows `abc`;
- after the program sends `\x1b[?1000h\x1b[?1006h`, a click sends a mouse report to the pty and selects nothing; the same click with Option held selects;
- on the alternate screen the wheel sends Up keys and the scroll bar does not move;
- `yes` running for 2 seconds: a `QTimer` in the test fires at least 20 times a second throughout, `scrollback()` is 10,000, and after the view is destroyed no `yes` process is left;
- new output under a selection drops it.

- [ ] **Step 2: Run them and see them fail**

Run: `make test-app`
Expected: FAIL.

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run the app's tests**

Run: `make test-app`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add app && git commit -m "app: the terminal scrolls back, selects, copies, pastes and passes the mouse on"
```

### Task 8: The terminal in the window

**Files:**
- Modify: `app/src/main_window.hpp`, `app/src/main_window.cpp`, `app/src/paths.hpp`, `app/src/paths.cpp`, `app/src/main.cpp`
- Test: `app/test/test_main_window.cpp`

**Interfaces:**
- Consumes: `TerminalView` (Tasks 6 and 7), `Board::quiet()` (Task 4).
- Produces in `paths`: `QString terminalCommand();` (`MOTH_TERMINAL`; unset gives `moth agent`, empty gives `""`), `QString userShell();` (`SHELL`, else `/bin/zsh`), and `QStringList terminalEnv(const QString &home, const QString &mothBin);` (the app's own environment with `MOTH_HOME` set to `home`, `MOTH_BIN` to `mothBin`, `PATH` with `mothBin`'s folder put first, `TERM=xterm-256color`, `COLORTERM=truecolor`, and `MOTH_ACTOR` removed).
- Produces on `MainWindow`: `TerminalView *terminal();` and `void setTerminal(bool on);` (on by default; off, for tests of the panels alone, builds no terminal and starts nothing).
- Object names: the view `terminal`, the checkbox `quiet`, the strip's label `terminalTitle`.

Behaviour:
- The splitter holds the column on the left and, on the right, a strip over the terminal view. The window opens 1,280 by 860 with the column 520 wide; the column never goes under 420 and the terminal takes the rest.
- The strip has a label and a `quiet` checkbox. The label shows the terminal's title when the program set one, else `terminal`. The checkbox is checked when `Board::quiet()` is true; clicking it runs `quiet on` or `quiet off` through `Board::run`, and it follows the board when someone else changes it.
- When the window is first shown the terminal runs, in the user's home folder with `terminalEnv`: `<userShell()> -l -i -c '<terminalCommand()>; exec "$SHELL" -l -i'`, or `<userShell()> -l -i` when the command is empty. So when the agent ends, or cannot start, a shell with `moth` on its `PATH` is left.
- When that shell itself exits the view keeps its last screen, the label reads `the terminal ended. press return in it to start it again.`, and Return in the view starts it again.
- Closing the window destroys the terminal first, so its child and everything in its process group get SIGHUP before the window is gone.
- The window's line, the panels and polling are as before; the terminal gets the focus when the window opens.

- [ ] **Step 1: Write the failing tests**

- `terminalCommand()` is `moth agent` with `MOTH_TERMINAL` unset, `echo hi` when set to that, and empty when set to empty; `terminalEnv` has `MOTH_HOME`, puts `mothBin`'s folder first in `PATH`, and has no `MOTH_ACTOR` even when the app's own environment has one;
- with `MOTH_TERMINAL='printf "from the terminal\n"'` the shown window's `terminal` has that text on its screen within 5 seconds, and then a shell prompt answers `echo $MOTH_HOME` with the home (one with a space in it);
- with `MOTH_TERMINAL='moth agent'` and `MOTH_BIN` pointing at a stub `moth` that prints `claude code is not installed, or not on PATH. moth works without it: use the window, or type moth commands here.` and exits 1 for `agent`: that sentence is on the screen and a shell prompt follows; the panels still answer (the search bar's `find` reaches the fake core);
- the `quiet` checkbox is checked after a status answer with `"quiet":true`; clicking it sends `quiet off` to the fake core; it is disabled while `Board::working()`;
- with a `sleep 300` started from the terminal's shell, closing the window leaves no shell and no `sleep` within 2 seconds;
- typing `exit` in the shell makes the label read the ended sentence; Return starts a shell again;
- `setTerminal(false)` before showing: no child process is started and `terminal()` is null;
- every earlier `test_main_window` test passes with `setTerminal(false)`.

- [ ] **Step 2: Run them and see them fail**

Run: `make test-app`
Expected: FAIL.

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run the app's tests and look at the real window once**

Run: `make test-app && make app`. Then, with a temporary `MOTH_HOME` and `MOTH_TERMINAL=` (a plain shell, no agent), open `bin/moth-app` for a few seconds and close it; say in the report what was seen. Do not run it on the default home.
Expected: all pass; the window shows the column and a shell prompt.

- [ ] **Step 5: Commit**

```bash
git add app && git commit -m "app: the terminal beside the panels, the agent started in it, and a quiet switch"
```

### Task 9: End to end, with a stand-in agent

**Files:**
- Create: `core/integration/stub_agent.sh` (the stand-in for `claude`)
- Modify: `core/integration/it_main.ml`, `app/test/it_app.cpp`, `docs/dev.md`

**Interfaces:**
- Consumes: everything above.
- `stub_agent.sh`: writes its arguments, one per line, to `$MOTH_HOME/stub-args`; runs the Stop hook's command from the settings file it was given, in the background, as Claude Code would; prints `stub agent up`; then, each time that hook command exits 2, appends its stderr to `$MOTH_HOME/stub-notes` and runs it again; ends when its stdin closes or it reads a line `bye`.
- `it_app` gains a mode `term <bottle> <exe> <number>`: opens the real window with the terminal on, waits for `stub agent up` on the terminal's screen, clicks through `find <number>` in the search bar, then prints `clicked`; when the harness tells it (a line on its stdin) it ticks `quiet` and prints `quiet`; on a second line it types `bye` and Return into the terminal, waits for a shell prompt, types `moth status` and Return, waits for `attached to` on the screen, prints `shell`, and closes the window.

Scenarios (numbered on from 36):
- **37, an agent's hand.** `MOTH_ACTOR=agent moth find 3` then `moth log --json`: the event's actor is `agent`. `MOTH_ACTOR=robot moth status` exits 2 with the Protocols text.
- **38, wait.** A session folder named for the harness's pid. `moth wait` started; `moth find 3` and `moth next 4` as the user, half a second apart: it exits 2 within 6 seconds and its stderr is the heading and the two steps in order. Started again and `MOTH_ACTOR=agent moth next 5` sent: still running after 3 seconds. `moth quiet on`, then a user `moth next 5`: still running after 3 seconds. `moth quiet off`, then a user `moth find 3`: exits 2 with that step alone.
- **39, the agent in the window.** `MOTH_CLAUDE=<stub_agent.sh>`, `it_app term`. After `clicked`: within 6 seconds `stub-notes` holds a note with `find <number>` in it, and `stub-args` holds `--settings` and a path under the home. After `quiet`: `status --json` has `"quiet":true`, and a `moth next <n>` from the harness adds nothing to `stub-notes` in 4 seconds. After `shell`: `it_app` has exited 0, and no `stub_agent.sh`, `moth wait`, shell or `moth-app` process is left.
- **40, no agent.** `MOTH_CLAUDE=/nonexistent`, `it_app term` in a variant that waits for the not-installed sentence instead of `stub agent up`: the sentence is on the screen, the click-through still fills the results list, and nothing is left running.

`docs/dev.md` gains: the terminal (`MOTH_TERMINAL`), the agent (`moth agent`, what is written under `agent/`, `MOTH_CLAUDE`), `moth wait` and its rule, `quiet`, `MOTH_ACTOR`, and scenarios 37 to 40.

- [ ] **Step 1: Write the scenarios and the `it_app` mode; run them and see what fails**

Run: `make helper && make test-integration`
Expected: 37 to 40 run; fix what they find in the code they exercise, and say in the report what each fix was.

- [ ] **Step 2: Run everything**

Run: `make test && make helper && make test-integration`, then `ps axo pid,args | grep -iE "moth daemon|moth-(helper|target|app)|moth wait|stub_agent|it_app"`.
Expected: all pass (12 skipped as documented); the `ps` shows only a live session's processes, if one is running.

- [ ] **Step 3: Commit**

```bash
git add core app docs/dev.md && git commit -m "integration: an agent's hand, wait, the stand-in agent in the window, and no agent at all"
```

### Task 10: Prove it on Disco Elysium, with Claude Code

This task needs Chris at the keyboard and uses his Claude Code account. Do not mark stage 2b done without it. Stop any `moth daemon` from an older build first.

**Files:**
- Modify: `docs/verification/disco-elysium.md` (add a dated "stage 2b" section)

- [ ] **Step 1: A short session without Chris.** With a temporary `MOTH_HOME` and the test target in `uhmm`, run `moth agent` once in a pty with the real Claude Code. Confirm, and write down: the first reply comes without anything typed; `moth status` ran with no permission prompt and its event's actor is `agent`; after a user `moth find 3` from another terminal the session wakes and answers the note; `moth` resolved in the agent's shell. Note any prompt Claude Code showed at its start (folder trust, login). End the session; check nothing is left running, and that `~/.claude/settings.json` and `~/.claude.json` have the checksums they had before. If the agent's shell did not find `moth`, or the instructions were not followed, fix that before Chris sits down.
- [ ] **Step 2: Open.** Chris opens `bin/moth-app` with the game running. Nothing is typed: the agent starts in the terminal and says where things stand.
- [ ] **Step 3: The design's example.** Chris picks the game, types a number the game shows and clicks `new hunt`, changes it in the game, narrows. He then types only `which one is it?`.
- [ ] **Step 4: The agent's hand.** Chris asks the agent to change a kept value. Then three quick clicks in a row in the window.
- [ ] **Step 5: Quiet.** Chris ticks `quiet`, does two steps, waits a minute, asks `what did i just do?`, then unticks it and does one more step.
- [ ] **Step 6: The terminal.** Half a line typed in the agent's prompt, then a click in a panel. Scroll back, select and copy a line, paste it, resize the window while the agent is printing, type an accented letter and an emoji. `/exit`, then `moth status` in the shell that is left, then `moth agent` to start it again.
- [ ] **Step 7: Left alone.** The window sits untouched for at least 30 minutes with the agent idle, then one click in a panel.
- [ ] **Step 8: Close.** Chris closes the window.
- [ ] **Step 9: Record and judge.** The bar:
  - the agent's first words come with nothing typed, and no global file of Chris's agent setup changed (checksums before and after);
  - after a step in the window the agent speaks within 15 seconds, unprompted, and names a next move that fits what happened;
  - `which one is it?` is answered from what the agent saw, with no question back about what Chris did;
  - the agent's write runs with no permission prompt and shows in the window within a second as `agent: …`;
  - three quick clicks bring one note, not three;
  - with `quiet` ticked nothing unprompted comes in a minute, the question is still answered (from `moth log`), and unticked, the next step is commented on;
  - the half-typed line is intact after a note arrives;
  - scrolling back, copying, pasting, resizing and typed non-ASCII all work (Chris's word, with what failed if anything);
  - after 30 idle minutes a click still brings a comment;
  - `moth-app` uses under 5% of one core with the agent idle (`ps -o %cpu`, three readings), and the game stays playable while the agent prints (Chris's word);
  - after the window closes, no `claude`, `moth wait` or shell started by it is left.
  If a bar is missed, record it as missed with what was seen; do not adjust the bar.
- [ ] **Step 10: Commit**

```bash
git add docs/verification/disco-elysium.md && git commit -m "verification: disco elysium, stage 2b"
```

---

## Amendments after the build and the whole-branch review

What was built differs from the text above in these places. Where they disagree, this section is what the code does. `docs/dev.md` has the full account.

### Protocols, as built

| item | as built |
| --- | --- |
| `proto` on every request | `moth` and the window send `"proto": 2` with every request and every `watch`. The service refuses, unrun and unrecorded, a request with none or another number: `this moth is from another build than its background service. stop the service and it starts again by itself: moth stop`. `stop` and `status` are always served. The stage 2a command line can therefore only run `status` and `stop` against this service. |
| what the agent is refused | From actor `agent` the service refuses: `set <#n>` and `keep <#n>` without `--at` (`give the row's address too, so the write cannot land on another row: add --at <the address moth results showed for it>`); `launch` of a bottle and path that `moth games` does not list, or with any argument after the path; `quiet on` and `quiet off` (`quiet is the person's switch`). `moth agent` run under `MOTH_ACTOR=agent` exits 1 with `this is already the agent's session`. The actor is what the client says it is; these limits stop a well-meaning agent, not a hostile program. |
| `quiet on` | also drops whatever `moth wait` is holding, so a step made just before going quiet is not told. |
| `moth wait` | exits 2 only when it prints a note; a usage error or an unexpected failure exits 1; everything else 0. It never starts the service, retries every second while none answers, and still tells what it holds. Its own status read is sent as actor `user`, unrecorded. The session's files are `<dir>/cursor` (`N` or `N quiet`) and `<dir>/waiter` (a pid; the newest waiter wins), both mode 0600. `MOTH_WAIT_TIMEOUT` shortens its receive limit for tests. |
| `moth agent` | checks in this order: not the agent's own shell, no arguments, the program (`MOTH_CLAUDE`, else `claude` on `PATH`), the service's `proto`. It then writes `<home>/agent/<pid>/` with `settings.json`, `instructions.md` and `cursor` (the newest event's number, so the first waiter tells what happened while the agent was starting), and runs the agent in `<home>/agent/work`. The hook's `timeout` is 86400. |
| `Client` | `call ?autostart ?record ?timeout`, `watch ?after ?started ?idle ?timeout`. |
| the stale check | the window counts a service with no `proto`, or another, as stale; a failed status read and a dropped feed clear it. The stale sentence holds the line even while a request runs. |

### The agent's instructions, as built

The lines about rows in Task 3's text were replaced, and one was added:

```
- to keep or change a row, give its number bare (3, not #3: the shell reads # as a comment) and its address: moth keep 3 <name> --at <the address moth results showed>, moth set 3 <number> --at <that address>. moth refuses a row from you without --at. a row listed by moth results --near has no number of its own: keep it by address, moth keep <0xaddr> <the number it shows> <name>. a kept value is changed by name: moth set <name> <number>.
- you cannot start a program the person has not started from moth before, and quiet is theirs to switch: moth refuses both from you.
```

### The terminal, as built

- The window starts the agent by moth's absolute path in single quotes (`'<abs moth>' agent`), not the word `moth`: a login shell reorders `PATH`. The child is `<shell> -l -i -c "<command>\n<resume line>\nexec \"$SHELL\" -l -i"`. The resume line brings a command stopped with Control+Z straight back, under sh and zsh.
- The terminal's environment drops `MOTH_ACTOR`, `TERM_PROGRAM_VERSION`, `TERM_SESSION_ID`, `TMUX`, `TMUX_PANE`, `ITERM_SESSION_ID`, `CLAUDECODE` and `CLAUDE_CODE_ENTRYPOINT`, and sets `TERM_PROGRAM=moth`.
- Keys: the terminal's Control is the key marked control; a lone Command+C, V or A copies, pastes or selects all and other Command chords are left alone. Option is Meta (Option+b sends `ESC b`); Option+Left and Right send `ESC b` and `ESC f`. Shift+Return sends `\x1b[13;2u`. Shift+Backspace sends `0x7f`.
- Rows pushed while the alternate screen is shown are not added to the scrollback. The scrollback holds 10,000 lines. A selection survives new output until its first line leaves; select-all is a state that output does not clear.
- A wheel notch scrolls 3 lines; pixel deltas are used only when the event has a scroll phase (a trackpad).
- Closing the window hangs up and, after half a second, kills both the shell and the terminal's foreground process group, and never waits past about 1.5 seconds.
- When the shell ends the terminal says `the terminal ended. press return in it to start it again.`

### Integration, as built

The stand-in agent is an OCaml program (`core/integration/stub_agent.ml`): it reads the hook command from the settings file and runs it with exactly the settings' `env`. `it_app term` takes the text to wait for and refuses to run without `MOTH_CLAUDE`. Scenario 41 was added (the window closed with an agent up that ignores the hang-up leaves nothing running). Scenarios 38, 39 and 41 run in a home whose path has a space.

### Rulings made during the build

| | ruling | cost |
| --- | --- | --- |
| R1 | every test and probe uses a temporary `MOTH_HOME` | none |
| R2 | no test or builder runs the real `claude` | the real hookup is first seen in Task 10 |
| R3 | terminal tests use `/bin/sh` and a temporary `HOME` (one test also runs zsh with its own `ZDOTDIR`) | his own zsh setup is first seen in Task 10 |
| R4, R5 | the stand-in is OCaml; `it_app term` takes the text to wait for | a second small test program |
| R6 | `quiet on` drops held events | one more rule in `Wait` |
| R7 | `moth wait`'s status read is actor `user` | a note can head a status line the person did not type |
| R8 | `moth wait` exits 2 only when telling | none |
| R9, R10 | a failed status read clears stale; the stale sentence shows even while working | one click can get through to an old service after a status failure |
| R11 | Option+Left and Right send `ESC b` and `ESC f` | a special case in `TermScreen::key` |
| R12 | the pty's final drain is kept but untested (macOS cannot reap the shell until its output is read) | an untested line |
| R13 | Shift+Backspace `0x7f`; Shift+Return stays `\x1b[13;2u` | Shift+Return prints junk in a plain shell |
| R14 | Option is Meta | Option dead keys do not type accents; on British, German, French, Spanish, Italian and Nordic layouts the characters Option types (`# @ [ ] { } \| \\ ~`, by layout) cannot be typed in the terminal |
| R15 | pushes on the alternate screen are dropped | rows lost on a shrink while vim or less is open |
| R16, R17 | selection kept at the scrollback limit; Command+A is a state | a little arithmetic and a flag |
| R18 | the flood test allows one stall (20 paints a second, no gap over 250 ms) | a looser bound |
| R19 | the terminal starts moth by absolute path | the plan's `moth agent` test text changed |
| R20 | command and `exec` joined by a newline | none |
| R21 | the launching terminal's variables are dropped | a variable not listed still leaks |
| R22 | the agent may not `launch` what the person never launched, pass a game arguments, switch `quiet`, or start a nested `moth agent` | the agent cannot start a game for the first time |
| R23 | the agent's row writes need `--at` | the instructions text changed |
| R24 | `proto` on every request | the 2a command line stops working against this service, except `status` and `stop` |

Decided and only noted: two agent sessions on one home are allowed; login shells other than sh, bash and zsh are not supported; the agent may `moth stop` and `moth detach`; `claude --resume` run by hand has none of moth's settings.

### Task 10, changes

Step 1 also tries: whether the `Stop` hook runs after a turn is interrupted with Escape, and whether a hook on `SessionStart` with `asyncRewake` could arm the waiter instead. `~/.claude/settings.json` is compared by checksum; `~/.claude.json` is judged by reading its diff, because Claude Code rewrites it on every run.

To look at with Chris, beside steps 2 to 9:

- at the start: the folder-trust prompt in `agent/work` (moth must not answer it), how long the first turn takes with his own Claude Code setup loading, and `type -a moth` and `echo $MOTH_ACTOR $MOTH_HOME` in the agent's shell;
- how a note shows (`Stop hook feedback`) and that it is taken as his action; whether the agent adds `--at` to a row write; a click while the agent is mid-turn; Escape during an unprompted comment, then a click;
- keys: Shift+Return, Option+Return and Control+J each make a new line in the agent's prompt; Option+e sends `ESC e`; holding a letter key; the press-and-hold accent menu replaces its letter and opens at the cursor, as does the emoji picker; a paste of several lines arrives as one paste;
- the cursor becomes an outline when the window loses focus; a mouse wheel and the trackpad both scroll sensibly;
- Control+Z in the agent comes straight back; `/exit` with a waiter armed leaves no `moth wait` within a second, and the strip's title afterwards; exactly one `moth wait` after several turns and after the 30 idle minutes;
- closing the window in the middle of a reply leaves no `claude` and no `moth wait`;
- CPU with the prompt idle and with a spinner running.

### Known limits, kept

- After a turn is interrupted, and after 24 hours idle, nothing is told until the person types to the agent.
- Control+C before the agent is up can end the terminal; Return starts it again.
- A step made in the few milliseconds between `moth agent`'s status read and its first watch is not told, and a `quiet on` made in them is missed for that session.
- `moth agent` needs a free watcher place (16 at most) to read where the log stands.
- A waiter of this build cannot tell that the service is from a later build: it asks again every second until its session ends. To be fixed when the protocol number next changes.
- `Daemon.protocol` stayed 2, so a `moth` built on this branch before the fix wave takes this service for its own build and is then refused. No such build is in use.
- Under zsh the 700th Control+Z in one agent session ends the terminal (zsh's limit on nested functions).
- The agent's refused `launch` names `<bottle> <path>` in general, not the ones asked for.
- `events.jsonl` grows without bound and a watch replays at most 1,000 events; to be looked at in stage 3.
- Left from the whole-branch review as minors: zsh leaves the start line on screen when the agent dies of Control+C; a status read near the 30-second limit can make the service let the waiter go for one retry; two test programs are timing-sensitive on a loaded machine (`test_core_client` `aHungStartIsGivenUpOn`, `test_results` `itStopsAtTwentyThousand`).

---

## Not in this plan

- Hiding or sorting rows by value in a hunt with no number (found in stage 1c and again in stage 2a). It is engine work and gets its own small plan.
- Whether a window that is completely covered by another app keeps up. It was right the moment it was shown; macOS does not draw a covered window, so a picture of one proves nothing either way.
- Agents other than Claude Code. `MOTH_TERMINAL` lets another one be started in the terminal; nothing wires it to the hunt.
- A settings file or a packaged `.app`. The switches here are environment variables, like `MOTH_HOME`.
- Still deferred from stage 2a: time limits on the window's requests, the daemon's blocking replies, looking in a bottle without starting wine, a launch that fails inside the bottle.
- A guess at what each result is and restart-proof paths (stage 3), toggles that hold a value (stage 4), the overlay (stage 5).
