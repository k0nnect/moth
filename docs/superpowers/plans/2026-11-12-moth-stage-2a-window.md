# moth stage 2a: the window — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Mac window in which a whole hunt (pick or launch the game, find, narrow, list, keep, change, undo) is done by clicking, and which follows along within a second when the same session is driven from the `moth` command.

**Architecture:** The app (`app/`, C++20 on Qt 6 Widgets) is one more client of the core's socket and can do nothing the `moth` command cannot. The core gains a feed (`watch`) that pushes a line when any recorded request starts and when it ends, reads that leave no trace (for polling), a guard against acting on a row that has moved, and `launch`. The panels keep no state of their own: after every event on the feed they read `status`, `results` and `kept` again.

**Tech Stack:** OCaml 5.5.2 with dune, yojson, alcotest (as before); C++20, CMake, Qt 6.12 `qtbase` from Homebrew (Core, Gui, Widgets, Network, Test), linked dynamically.

**Spec:** `docs/superpowers/specs/2026-11-11-moth-design.md` (sections 6.1, 6.3, 7, 8, 9). The toolkit test it asks for is written up in `docs/notes/2026-11-12-app-toolkit-test.md`; read that too.

Stage 2 is built as two plans. This one is the window and its panels. Stage 2b (the built-in terminal, the agent started in it, and the agent speaking up) gets its own plan when this one is proven; its outline is at the end.

## Global Constraints

- moth's own source is OCaml and C++. Objective-C++ is allowed only as thin glue for Mac-only window behaviour; this plan needs none.
- No dependency under the GPL. Qt is used from `qtbase` only and linked dynamically. No other new library.
- The user never chooses a data type. No control names one. A value's kind is shown with the core's own tag text (`whole number`, `decimal`, ...), unchanged.
- Every control works with no agent running.
- No silent failure. Every error the core answers with is shown in the window, in the core's words.
- The app talks to the core's socket only: never to the helper, the game or `events.jsonl`.
- The app never blocks its interface on a request. A first scan takes about 10 seconds; the window must stay movable and scrollable during it.
- Polling reads are sent with `record: false` so they do not fill the event log.
- Text in the window is lowercase plain words, and uses the command's vocabulary: `find`, `narrow`, `went down`, `went up`, `same`, `changed`, `keep`, `kept`, `forget`, `undo`.
- Writing to the wrong place is the worst failure: a click on a row acts on the address that row showed, or on nothing.
- Automated tests use only the `uhmm` bottle and a temporary `MOTH_HOME`, leave no `moth daemon`, `moth-helper`, `moth-target` or `moth-app` process behind, and run windows with `QT_QPA_PLATFORM=offscreen`.
- SIP stays on, no `sudo`, CrossOver is not re-signed, nothing is injected into a game, single-player and offline only.
- Commits: lowercase message, no attribution or co-author trailer. Local git only; no remote is added and nothing is pushed.
- If a test's expectation has to change because of this plan (for example `status --json` gaining fields), change it in the task that causes it and say so in the report.

## Review Focus

1. **A click while a scan is running** (the user's, or one somebody else started): the controls that would start another request are disabled, nothing is queued twice, and the window says what is running.
2. **The row moved.** Someone narrows the hunt between the list being drawn and the user typing over a value in row 3: the write is refused with the core's message and the list is read again. It never lands on whatever is in row 3 now.
3. **The game closes mid-hunt:** the window says the game closed, the list and the kept values empty, and no control can write.
4. **The background service stops or is restarted** from a terminal while the window is open: the window says it lost it, reconnects by itself, and the next click works.
5. **Sixteen million results:** the list shows the first page at once and fetches more only when scrolled; memory and the core are not asked for all of them.

Each has a test in the task that owns the code (Tasks 4, 2 and 6, 4, 4, 6).

## Protocols

Additions to the client protocol (newline-delimited JSON on `$MOTH_HOME/core.sock`). Everything not listed is unchanged.

| item | shape | rule |
| --- | --- | --- |
| request field `record` | boolean, default true | `false` is allowed on `bottles`, `ps`, `status`, `results`, `kept`, `log`, `games`. The request is answered, takes no `seq`, adds no line to `events.jsonl` and sends nothing to watchers. On any other command: error `only commands that change nothing can go unrecorded`. Exception: a `status` that finds the game closed is recorded even when asked not to be. |
| command `watch` | opts: `after` (a seq, optional) | Reply `{"id":..,"ok":true,"result":{"seq":N},"text":""}` with N the newest recorded seq. The connection then stays open and the daemon writes feed lines to it. With `after` = k, the stored events with seq > k (at most the newest 1,000, oldest first) are written right after the reply. Never recorded. At most 16 watchers; the next gets error `too many watchers`. |
| feed line, start | `{"busy":{"actor":"..","cmd":"..","args":[..]}}` | written when a recorded request starts being handled |
| feed line, end | `{"event":{...}}` | the object appended to `events.jsonl` for that request, written after it |
| watcher that falls behind | | a watcher whose socket cannot take a line at once is closed; it reconnects with `after`. A request is never delayed by a watcher. |
| `status --json`, `hunt` | adds `"by_tag":[["whole number",241178],..]` and `"can_compare":true` | `by_tag` as in a step's outcome; `can_compare` is `Session.can_compare` |
| `kept --json`, each item | adds `"note":".."` | the words `moth kept` prints after the tag for that value, or `""` |
| option `--at <0xaddr>` | on `keep <#n> [name]` and `set <#n> <number>` | the row must be at that address. Otherwise: `row <n> is not at <0xaddr> any more: the list changed. look again: moth results`. With a name instead of a row: `--at goes with a row number`. Not an address: `--at needs an address like 0x7ffe1234`. |
| command `launch` | `launch --bottle <b> <full path to .exe> [args...]` | starts the program in the bottle, detached, output to `$MOTH_HOME/launch.log`, and does not wait for it. Remembers it in `$MOTH_HOME/games.json`. |
| command `games` | no arguments | what `launch` has started, newest first, at most 20, one per bottle and path |

---

### Task 1: The feed

**Files:**
- Modify: `core/lib/daemon.ml`, `core/lib/daemon.mli`, `core/lib/client.ml`, `core/lib/client.mli`, `core/bin/main.ml`, `docs/dev.md`
- Test: `core/test/test_daemon.ml`

**Interfaces:**
- Produces: the `record` field, `watch` and the two feed lines, exactly as in Protocols.
- Produces: `Client.watch : home:string -> ?after:int -> (Yojson.Safe.t -> bool) -> (unit, string) result`. It sends `watch`, then calls the function with each feed line (the whole object) until the function returns false or the daemon closes the connection. It does not start a daemon: with none answering it returns `Error Client.not_running`.
- Produces: `moth watch [--after <seq>] [--json]`. Text mode prints an event as `moth log` prints one, and a start as `working: <cmd> <args> (<actor>)`. `--json` prints the feed lines as they come. It ends when the daemon goes, printing `moth stopped`. Help line, after `log`: `watch [--after <seq>]                watch commands as they happen, from anyone`.

- [ ] **Step 1: Write the failing tests**

In `test_daemon.ml`, one test each:
- a watcher connected before a `find 3` from another connection receives a `busy` line with `actor` `"user"`, `cmd` `"find"`, `args` `["3"]`, then an `event` line whose object equals the line `events.jsonl` gained;
- `watch` with `after` two events back receives the reply, then those two events oldest first, then live lines;
- `watch` itself, and a `status` and a `results` sent with `record:false`, add no line to `events.jsonl`, leave `seq` where it was, and send watchers nothing;
- `find 3` with `record:false` answers `only commands that change nothing can go unrecorded` and the helper is not called;
- a `status` with `record:false` that finds the game gone is recorded and reaches watchers;
- with 16 watchers connected a 17th is answered `too many watchers`; after one disconnects, a new one is accepted and a `status` still answers;
- a watcher that never reads does not hold a request up: with its socket buffer full, a `status` from another connection answers within one second and the watcher's connection is closed;
- `Client.watch` with no daemon returns `Error Client.not_running`.

- [ ] **Step 2: Run them and see them fail**

Run: `opam exec -- dune test`
Expected: FAIL (`unknown command: watch`).

- [ ] **Step 3: Implement**

The daemon stays one loop serving one request at a time. Watchers join the `select` set so a closed one is noticed; writes to them are non-blocking.

- [ ] **Step 4: Run every unit test and build**

Run: `make test && make build && bin/moth help`
Expected: all pass; the help lists `watch`.

- [ ] **Step 5: Commit**

```bash
git add core docs/dev.md && git commit -m "moth: watch, a feed of commands as they start and end, and reads that leave no trace"
```

### Task 2: What the panels need from status, kept and a row

**Files:**
- Modify: `core/lib/session.ml`, `core/lib/session.mli`, `core/lib/render.ml`, `core/lib/render.mli`, `core/lib/daemon.ml`, `core/lib/client.ml`, `core/bin/main.ml`
- Test: `core/test/test_session.ml`, `core/test/test_daemon.ml`, `core/test/test_render.ml`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `by_tag` and `can_compare` in `status --json`'s `hunt`; `note` in each `kept --json` item; `--at` on `keep <#n>` and `set <#n>`. Exact shapes and texts are in Protocols.
- Produces: `Render.kept_note : Session.kept -> string`, the words after the tag in `moth kept` for one value (`""` when there are none). `Render.kept` uses it, so the text output does not change.
- `moth help`: the `keep <#n>` and `set` lines gain ` [--at <0xaddr>]`.

- [ ] **Step 1: Write the failing tests**

One test each:
- after `find 100` on the fake helper, `status --json`'s `hunt.by_tag` equals the `by_tag` the `find` returned, and `can_compare` is true; after a `find` whose groups kept no values it is false;
- `kept --json` after a `set` that went back has `"note":"you set it to 20; it is back to what it was before."`, and `""` for a value never set;
- `set 3 9 --at <row 3's address>` writes; with any other address it answers the Protocols text with that row and address, and the helper receives no write;
- `keep 3 hp --at <other address>` answers the same text and nothing is kept;
- `set hp 9 --at 0x10` answers `--at goes with a row number`; `set 3 9 --at 12` answers `--at needs an address like 0x7ffe1234`.

- [ ] **Step 2: Run them and see them fail**

Run: `opam exec -- dune test`
Expected: FAIL.

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run every unit test**

Run: `make test`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add core && git commit -m "moth: status says what a hunt holds, kept says its note, and a row can be pinned to its address"
```

### Task 3: launch and games

**Files:**
- Modify: `core/lib/daemon.ml`, `core/lib/daemon.mli`, `core/lib/client.ml`, `core/bin/main.ml`, `docs/dev.md`
- Test: `core/test/test_daemon.ml`

**Interfaces:**
- Produces: `type Daemon.launch = bottle:string -> exe:string -> args:string list -> (unit, string) result`; `Daemon.run ~home ~start_helper ~launch`; `Daemon.default_launch : home:string -> launch` (CrossOver's wine through `Crossover.wine_argv`, its own session, stdin from `/dev/null`, output appended to `[home]/launch.log`).
- Produces: `launch` and `games` as in Protocols, with these texts:
  - started: `started <name> in <bottle>. when it is up: moth attach --bottle "<bottle>" <name>`; JSON `{"message":..,"name":..,"bottle":..,"path":..}`.
  - `launch needs a bottle and a program: moth launch --bottle <bottle> <full path to game.exe>` (usage).
  - `launch needs the full path to a windows program (.exe): <what was given>` for a relative path or another extension (`.EXE` counts).
  - `no such file: <path>`.
  - an unknown bottle: `Crossover.check_bottle`'s message.
  - `games` with nothing remembered: `nothing launched from moth yet. start a game with: moth launch --bottle <bottle> <full path to game.exe>`; otherwise one line per game, `<name>  <bottle>  <path>` in aligned columns; JSON a list of `{"name","bottle","path"}`.
- `[home]/games.json`: a JSON list of `{"name","bottle","path","last"}`, newest first, one entry per bottle and path, at most 20. An unreadable file counts as empty and is replaced at the next launch.
- Help lines, after `attach`: `launch --bottle <bottle> <game.exe path> [args]  start a game in a bottle` and `games                                games started from moth before`.

- [ ] **Step 1: Write the failing tests**

With a fake `launch` that records its calls, one test each:
- `launch --bottle uhmm /tmp/x/game.exe -windowed` (the file exists) calls it once with bottle `uhmm`, that path and `["-windowed"]`, answers the started text, and `games` then lists it first;
- the same launch twice leaves one entry; 21 different ones leave 20, the oldest gone;
- a missing file, a relative path, a `.txt` and a missing `--bottle` each give their text and never call it;
- `games` on a home with a corrupt `games.json` answers the nothing-yet text.

- [ ] **Step 2: Run them and see them fail**

Run: `opam exec -- dune test`
Expected: FAIL (`unknown command: launch`).

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run every unit test and build**

Run: `make test && make build && bin/moth help`
Expected: all pass; the help lists `launch` and `games`.

- [ ] **Step 5: Commit**

```bash
git add core docs/dev.md && git commit -m "moth: launch a game in its bottle, and games lists the ones started before"
```

### Task 4: The app's skeleton and its line to the core

**Files:**
- Create: `app/CMakeLists.txt`, `app/src/main.cpp`, `app/src/paths.hpp`, `app/src/paths.cpp`, `app/src/core_client.hpp`, `app/src/core_client.cpp`, `app/src/board.hpp`, `app/src/board.cpp`, `app/src/main_window.hpp`, `app/src/main_window.cpp`
- Create: `app/test/fake_core.hpp`, `app/test/fake_core.cpp`, `app/test/test_core_client.cpp`, `app/test/test_board.cpp`
- Modify: `Makefile` (targets `app` and `test-app`; `test` runs `test-app` too), `docs/dev.md` (the `qtbase` toolchain line, how to build and run the app)

**Interfaces:**
- Consumes: Tasks 1 and 2 (the feed, `record`, the new `status` fields).
- Produces the build: `make app` configures `app/` into `build/app` with `-DCMAKE_PREFIX_PATH=$(brew --prefix qtbase)` and copies the program to `bin/moth-app`. `make test-app` builds the tests and runs them with `QT_QPA_PLATFORM=offscreen` through `ctest --output-on-failure`.
- Produces `paths.hpp`: `QString mothHome()` (`MOTH_HOME` made absolute, else `~/Library/Application Support/moth`, the rule `Client.home` uses) and `QString mothBin()` (`MOTH_BIN`, else `moth` next to the running program).
- Produces `core_client.hpp`:

```cpp
using Opts = QList<QPair<QString, QString>>;
struct Reply { bool ok = false; QJsonValue result; QString text; QString error; };

class CoreClient : public QObject {
  Q_OBJECT
public:
  CoreClient(QString home, QString mothBin, QObject *parent = nullptr);
  // one request on its own connection, actor "user"; `done` runs once, on the caller's thread
  void call(const QString &cmd, const QStringList &args, const Opts &opts, bool record,
            std::function<void(const Reply &)> done);
  void startFeed();  // `watch`; when it drops, tries again every second with `after` = the last seq seen
signals:
  void busy(const QJsonObject &what);   // a recorded request started
  void event(const QJsonObject &ev);    // a recorded request ended
  void feedUp(bool up);
};
```

  When a connection is refused, the client runs `mothBin status` once (which starts the daemon), waits up to 10 seconds for it, and tries the request again. If that fails too, `done` gets `error` = `moth's background service is not answering. see <home>/core.log`.
- Produces `board.hpp`, the one object every panel reads from:

```cpp
struct Attached { int pid; QString name; int bits; QString bottle; };
struct Hunt { qint64 main; qint64 wide; QList<QPair<QString, qint64>> byTag; bool blind; bool canCompare; };

class Board : public QObject {
  Q_OBJECT
public:
  explicit Board(CoreClient *core, QObject *parent = nullptr);
  const std::optional<Attached> &attached() const;
  const std::optional<Hunt> &hunt() const;
  QString counts() const;    // status's text after its first line; "" with no hunt
  bool working() const;      // a recorded request is running, ours or anyone's
  QString what() const;      // "find 100 (agent)" while working
  bool connected() const;
  QString line() const;      // the newest thing to tell the user
  void say(const QString &line);
  // a recorded request as the user. Refused while working: line becomes "moth is busy: <what>"
  void run(const QString &cmd, const QStringList &args = {}, const Opts &opts = {},
           std::function<void(const Reply &)> then = {});
  // an unrecorded read. Dropped (and `done` never called) while working, or when a read with the
  // same command, arguments and options is still out
  void read(const QString &cmd, const QStringList &args, const Opts &opts,
            std::function<void(const Reply &)> done);
  void tick();               // the once-a-second poll; the window calls it from a timer while visible
signals:
  void changed();            // anything the getters return changed
  void turned();             // a recorded request ended (anyone's): lists read themselves again
  void ticked();             // a poll: lists refresh the values they show
};
```

  Rules: a `busy` line sets `working`. An `event` clears it, sets `line` to the event's `summary` (prefixed `agent: ` when its `actor` is not `user`), reads `status` and emits `turned`. A failed `run` sets `line` to the error. A `status` reply with `closed` sets `line` to its text and clears `attached` and `hunt`. The feed going down sets `line` to `lost moth's background service. trying again…`; coming back reads `status`.
- Produces `main_window.hpp`: `class MainWindow : public QMainWindow { explicit MainWindow(Board *board); QVBoxLayout *column(); }`. The central widget is a `QSplitter` holding one column (stage 2b adds the terminal beside it). Under the column a `QLabel` named `line` shows `Board::line()`, or `working: <what>…` while working. Title `moth`, first size 560 by 860. A one-second timer calls `Board::tick()` while the window is visible.
- Produces `fake_core.hpp`, for every app test: a `QLocalServer` at `<temp home>/core.sock` speaking the protocol.

```cpp
class FakeCore : public QObject {
public:
  explicit FakeCore(const QString &home);
  void on(const QString &cmd, std::function<Reply(const QStringList &args, const Opts &opts)> answer);
  void hold(const QString &cmd);                 // answer this command only when release() is called
  void release(const QString &cmd);
  void push(const QJsonObject &feedLine);        // to every watcher
  void dropWatchers();
  void stop(); void start();                     // the service going away and coming back
  struct Seen { QString actor, cmd; QStringList args; Opts opts; bool record; };
  QList<Seen> seen() const;                      // every request but watch, in order
  int watchers() const;
};
```

- [ ] **Step 1: Write the failing tests**

`test_core_client.cpp`:
- `call("results", {}, {{"all","true"},{"n","200"}}, false, …)` reaches the fake as cmd `results` with those opts (the keys the command line's `--all` and `-n` become), `record` false and actor `user`; a recorded call carries no `record` field, or `true`;
- an error reply arrives as `ok` false with the core's text in `error`;
- after `startFeed`, a pushed `busy` then `event` arrive as the two signals in that order;
- when the fake drops its watchers, `feedUp(false)` is emitted, and within two seconds the client is watching again with `after` equal to the last event's seq;
- with no server at the socket and `MOTH_BIN` set to a script that only writes a marker file, a call runs the script once and then reports `moth's background service is not answering. see <home>/core.log`.

`test_board.cpp`:
- (Review Focus 1) after a `busy` line from actor `agent` for `find 100`, `working()` is true, `what()` is `find 100 (agent)`, and `run("next", {"down"})` sends nothing and sets `line()` to `moth is busy: find 100 (agent)`;
- an `event` with actor `agent` and summary `241,178 matches` gives `line()` `agent: 241,178 matches`, one `status` read with `record` false, and `turned` once;
- (Review Focus 3) a `status` reply `{"attached":null,"hunt":null,"closed":"disco.exe"}` with text `not attached. disco.exe closed.` clears `attached()` and `hunt()` and sets that line;
- (Review Focus 4) `FakeCore::stop()` makes `connected()` false and the line `lost moth's background service. trying again…`; after `start()` it is true again within two seconds and a `run("status")` succeeds;
- two `tick()` calls while the first `status` read is held send one `status`;
- `hunt()` carries `byTag`, `blind` and `canCompare` from a `status` reply shaped as in Protocols.

- [ ] **Step 2: Run them and see them fail**

Run: `make test-app`
Expected: FAIL to build (no `app/src`).

- [ ] **Step 3: Implement**

`QLocalSocket` for both the calls and the feed; no thread. No `Q_OBJECT` class in a header that the tests do not need.

- [ ] **Step 4: Run the tests and open the window**

Run: `make test-app && make app && bin/moth-app`
Expected: tests pass; a window titled `moth` opens showing `not attached` in its line, and `bin/moth status` typed in a terminal makes the line change within a second. Close it; `ps axo args | grep moth-app` shows nothing.

- [ ] **Step 5: Commit**

```bash
git add app Makefile docs/dev.md && git commit -m "app: a window that talks to the core and follows its feed"
```

### Task 5: The game bar

**Files:**
- Create: `app/src/game_bar.hpp`, `app/src/game_bar.cpp`, `app/test/test_game_bar.cpp`
- Modify: `app/src/main_window.cpp`, `app/CMakeLists.txt`

**Interfaces:**
- Consumes: `Board`, `FakeCore` (Task 4); `launch`, `games` (Task 3).
- Produces: `class GameBar : public QWidget { explicit GameBar(Board *); void attach(const QString &bottle, const QString &name); void launch(const QString &bottle, const QString &path); void detach(); }`, first in the window's column.
  - A label named `game`: `no game attached`, or `<name> · <bottle> · <bits>-bit`.
  - A button `pick a game` opening a menu: one submenu per bottle (from `bottles`), filled when it opens from `ps --bottle <b>` (read unrecorded; a bottle with nothing running shows the core's text as a disabled entry). Choosing a program calls `attach`.
  - In the same menu, under a separator: the remembered `games` (`<name> · <bottle>`), then `another game…`, which asks for an `.exe` with a file dialog and for the bottle with a list. Both call `launch`.
  - A button `detach`, enabled when attached.
  - After a successful `launch`, the bar reads `ps --bottle <b>` every 2 seconds for up to 90 seconds. The line says `waiting for <name> to start…`. When a program with that name (letter case ignored) is listed, it calls `attach`. If none appears: `<name> did not show up in <bottle> after 90 seconds. pick it once it is running.`
  - `pick a game` and `detach` are disabled while `Board::working()`.

- [ ] **Step 1: Write the failing tests**

- `attach("uhmm", "game.exe")` sends recorded `attach` with args `["game.exe"]` and opts `bottle=uhmm`; after the event and a `status` reply naming it, the label reads `game.exe · uhmm · 64-bit` and `detach` is enabled;
- an `attach` the core refuses (any error text) leaves the label at `no game attached` and the window's line showing that text unchanged;
- `launch("uhmm", "/tmp/x/Game.EXE")` sends `launch`; when the second `ps` read lists `game.exe`, an `attach` for it follows; with the waiting period shortened by a test-only setter and nothing listed, no `attach` is sent and the line is the did-not-show-up text;
- `detach()` sends `detach`, and after a `status` reply with `attached` null the label is `no game attached`;
- while a `busy` line is in force both buttons are disabled.

- [ ] **Step 2: Run them and see them fail**

Run: `make test-app`
Expected: FAIL to build.

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run the tests**

Run: `make test-app`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app && git commit -m "app: pick a running game, or launch one and attach when it is up"
```

### Task 6: Search and results

**Files:**
- Create: `app/src/search_bar.hpp`, `app/src/search_bar.cpp`, `app/src/results_model.hpp`, `app/src/results_model.cpp`, `app/src/results_panel.hpp`, `app/src/results_panel.cpp`, `app/test/test_search_bar.cpp`, `app/test/test_results.cpp`
- Modify: `app/src/main_window.cpp`, `app/CMakeLists.txt`

**Interfaces:**
- Consumes: `Board`, `FakeCore`; `--at` and the `status` fields (Task 2).
- Produces `SearchBar(Board *)`, second in the column, with these named widgets:

| name | kind | text | does |
| --- | --- | --- | --- |
| `number` | line edit | placeholder `the number the game shows` | Return narrows when a hunt is in progress, otherwise starts one |
| `hunt` | button | `new hunt` | `find <number>` |
| `narrow` | button | `narrow` | `next <number>` |
| `snapshot` | button | `no number` | `snapshot` |
| `down`, `up`, `same`, `changed` | buttons | `went down`, `went up`, `same`, `changed` | `next down`, `next up`, `next same`, `next changed` |
| `counts` | label | `Board::counts()` | |

  The number is sent as typed, trimmed. With nothing typed, `hunt` and `narrow` send nothing and the line says `type the number the game shows`. Everything is disabled with no game attached or while working. `narrow` needs a hunt. The four relation buttons need a hunt with `canCompare`. After a step that worked, the box's text is selected so the next number replaces it.
- Produces `ResultsModel(Board *)`, a table model with columns `#`, `value`, `kind`, `address`:
  - `void reload()`: forgets its rows and reads `results` from row 1 (`n` 200, plus `all` when `setAll(true)`), unrecorded. Called on `Board::turned` and when `all` changes.
  - It fetches the next 200 through Qt's `canFetchMore` and `fetchMore`, and stops at 20,000 rows.
  - `void refresh(int firstRow, int lastRow)`: reads again the pages those rows are on and updates values in place. If a page comes back with a different address in a row it holds, it reloads.
  - Typing over a `value` sends recorded `set <#n> <text> --at <that row's address>`. On an error the cell keeps its old value and the line shows the error; on the row-moved error it also reloads.
  - `void keep(int row, const QString &name)`: `keep <#n> [name] --at <address>`.
  - `qint64 total() const`.
- Produces `ResultsPanel(Board *)`, third in the column: the table, a checkbox `show rarer forms` (`all`), a button `keep` (enabled with a row selected; asks for an optional name; `void keepSelected(const QString &name)` is what it calls), and a label named `shown`: `<n> matches`, `showing <rows> of <total>`, `showing the first 20,000 of <total>. narrow the hunt to see fewer.`, `nothing to list yet` for a snapshot no step has narrowed, and empty with no hunt. Numbers have thousands separators. On `Board::ticked` it refreshes the rows in view.

- [ ] **Step 1: Write the failing tests**

`test_search_bar.cpp`:
- with a game attached and no hunt, typing ` 100 ` and Return sends `find` with args `["100"]`; with a hunt it sends `next`;
- with nothing typed, `hunt` sends nothing and the line is `type the number the game shows`;
- `went down` sends `next` `["down"]`; the four relation buttons are disabled when `can_compare` is false, and `narrow` when there is no hunt;
- (Review Focus 1) during a held `find`, every button is disabled and a second Return sends nothing;
- an error reply (`132.5.1 is not a number` or whatever the fake answers) appears as the line, word for word;
- `no number` sends `snapshot`.

`test_results.cpp`:
- (Review Focus 5) with a fake answering `total` 16,700,000, `reload()` sends one `results` request (`n` 200, `from` absent or 1, `record` false) and `rowCount()` is 200; the label reads `showing 200 of 16,700,000`; `fetchMore` sends one more with `from` 201;
- a row shows the fake's `shown`, `tag` and `addr`;
- (Review Focus 2) editing row 3's value to `9` sends `set` with args `["3","9"]` and opts `at=<row 3's address>`; when the fake answers the row-moved text, the line shows it and one new `results` read follows; the cell shows the re-read value;
- `keepSelected("hp")` with row 2 selected sends `keep` `["2","hp"]` with `at`;
- `refresh` with a page whose row 5 changed value updates that cell and emits `dataChanged` for it only;
- (Review Focus 3) after `turned` with a `status` showing no game, `rowCount()` is 0 and the label is empty;
- `show rarer forms` adds `all` to the next read.

- [ ] **Step 2: Run them and see them fail**

Run: `make test-app`
Expected: FAIL to build.

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run the tests**

Run: `make test-app`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add app && git commit -m "app: search with no type to choose, and a results list that reads itself"
```

### Task 7: Kept values and undo

**Files:**
- Create: `app/src/kept_panel.hpp`, `app/src/kept_panel.cpp`, `app/test/test_kept_panel.cpp`
- Modify: `app/src/main_window.cpp`, `app/CMakeLists.txt`

**Interfaces:**
- Consumes: `Board`, `FakeCore`; `kept --json`'s `note` (Task 2).
- Produces `KeptPanel(Board *)`, last in the column: a table with columns `name`, `value`, `kind`, `note`, read from `kept` (unrecorded) on `Board::turned` and on `Board::ticked`.
  - Typing over a `value` sends recorded `set <name> <text>`; an error leaves the old value and shows in the line.
  - Buttons `forget` (the selected value; `forget <name>`) and `undo last change` (`undo`), both disabled while working; `forget` needs a selection.
  - With nothing kept, in the table's place: `nothing kept yet. select a result and press keep.`
  - While a cell is being edited, a refresh does not replace its text.
- This stage has no toggle that holds a value. Holding belongs to stage 4 (design section 6.6); the panel shows live values and takes typed ones.

- [ ] **Step 1: Write the failing tests**

- two kept values from the fake appear with name, `shown`, `tag` and `note`;
- editing `points` to `5` sends `set` with args `["points","5"]` and no `at`;
- a refresh while `points` is being edited leaves the editor's text alone and still updates the other row;
- `forget` with `xp` selected sends `forget` `["xp"]`; `undo last change` sends `undo`, and the core's answer (`nothing to undo`, or what it put back) becomes the line;
- an empty `kept` shows the nothing-kept text;
- after a `status` showing no game, the table is empty.

- [ ] **Step 2: Run them and see them fail**

Run: `make test-app`
Expected: FAIL to build.

- [ ] **Step 3: Implement**

- [ ] **Step 4: Run the tests and look at the window**

Run: `make test-app && make app`
Expected: PASS. `bin/moth-app` shows, top to bottom: the game bar, the search bar, the results list, the kept values, the line.

- [ ] **Step 5: Commit**

```bash
git add app && git commit -m "app: kept values that can be typed over, forget, and undo"
```

### Task 8: End to end in the bottle

**Files:**
- Create: `app/test/it_app.cpp`
- Modify: `app/CMakeLists.txt`, `core/integration/it_main.ml`, `core/integration/target_ctl.ml`, `Makefile` (`test-integration` depends on `app`), `docs/dev.md`

**Interfaces:**
- Consumes: everything above.
- Produces in `Target_ctl`: a way to adopt a target that moth launched: listen first, hand the port to the caller, accept afterwards. Existing callers are unchanged.
- Produces `build/app/it_app <edit|follow> <bottle> <exe name> <number> <0xaddr> <new number>`: with `MOTH_HOME` and `MOTH_BIN` from the environment and `QT_QPA_PLATFORM=offscreen`, it builds the real `MainWindow` against the real daemon, and prints one line per step, then `ok`, exiting 0; any failed step prints what it expected and exits 1. Both modes attach, search for `<number>` and find the row at `<0xaddr>`, then print `listed`. `edit` types `<new number>` over that row and keeps it. `follow` changes nothing itself: it waits up to 30 seconds for that row to read `<new number>`.

- [ ] **Step 1: Add the scenarios**

| # | name | steps and checks |
| --- | --- | --- |
| 31 | the feed | a second client runs `watch`; `find 3` from the first: the watcher sees `busy` then `event`, the event's `seq` equal to `moth log`'s newest. `status` with `record:false` afterwards: the watcher sees nothing for a second. |
| 32 | launch | `launch --bottle <test bottle> <bin>/moth-target.exe <port>`: the target connects to the harness within the usual timeout; `games` lists it; `attach` by name works; the target is told to quit. |
| 33 | the window | target running with `points` 3. Run `it_app edit <bottle> moth-target.exe 3 <points addr> 9`. Inside it: `GameBar::attach`; wait for the label; type `3`, Return; wait until not working; the results model has rows and `total()` > 0; `show rarer forms` off; find the row whose address is `<points addr>` (fetching more as needed, at most 20,000); edit its value to `9`; `keepSelected("points")` on it; the kept table shows `points` `9`. Then the harness asks the target: `points` is 9. |
| 34 | the list follows | `it_app follow <bottle> moth-target.exe 3 <points addr> 4`. When it prints `listed`, the harness sets `points` to 4 in the target and runs `moth next 4` from the command line. `it_app` sees the list read itself again and the row at `<points addr>` show `4`, without anything being clicked. |

- [ ] **Step 2: Run them**

Run: `make helper && make test-integration` (twice)
Expected: scenarios 1 to 11 and 13 to 34 `ok`, 12 the documented skip; afterwards `ps axo pid,args | grep -iE "moth daemon|moth-(helper|target|app)|it_app"` shows nothing.

- [ ] **Step 3: Commit**

```bash
git add app core Makefile docs/dev.md && git commit -m "integration: the feed, launch, the window against a real game, and a row that moved"
```

### Task 9: Prove it on Disco Elysium

This task needs Chris at the keyboard. Do not mark stage 2a done without it. Stop any `moth daemon` from an older build first (its kept values are forgotten; keep them again by address).

**Files:**
- Modify: `docs/verification/disco-elysium.md` (add a dated "stage 2a" section)

- [ ] **Step 1: Open and attach.** `bin/moth-app`. Chris picks `disco.exe` from `pick a game`. If the game is not running, he launches it from the same menu and the window attaches by itself.
- [ ] **Step 2: A hunt by clicking.** Experience: type the number, `new hunt`; gain experience; type the new number, Return. Keep the row, type over its value, see it in the game.
- [ ] **Step 3: A hunt with no number.** A thought's research time: `no number`, let game minutes pass, `went down`, and so on, as in stage 1c.
- [ ] **Step 4: Both hands on one session.** With the window open, run `bin/moth set points 5` in a terminal: the kept panel shows 5 and the line shows the command within a second. Run `bin/moth find 1` in the terminal and click in the window while it runs.
- [ ] **Step 5: The game goes away.** Chris quits the game with the window open.
- [ ] **Step 6: Record and judge.** The bar:
  - steps 2 and 3 done with no command typed anywhere;
  - during a first scan the window can be moved and the kept panel scrolled (Chris's word);
  - a command from the terminal shows in the window within one second;
  - while a scan runs, a click in the window starts nothing and the window says what is running;
  - after the game quits, the window says so within two seconds and nothing can be written;
  - with a list in view and nothing happening, `moth-app` uses under 5% of one core (`ps -o %cpu`, three readings).
  If a bar is missed, record it as missed with what was seen; do not adjust the bar.
- [ ] **Step 7: Commit**

```bash
git add docs/verification/disco-elysium.md && git commit -m "verification: disco elysium, stage 2a"
```

---

## What stage 2b will be (outline, not part of this plan)

Settled by the toolkit test, so they are not open questions:

- The terminal is libvterm's screen painted by a Qt widget over a pty. It goes in the splitter beside the column this plan builds.
- The agent is started by a new `moth agent` command, which writes a per-session settings file and instructions under `$MOTH_HOME` and runs `claude --settings <file> --append-system-prompt-file <file> --allowedTools "Bash(moth *)"` with a first prompt. Nothing global is edited. The app's terminal runs `moth agent`, and falls back to a shell when the agent is not installed.
- The agent uses the `moth` command. Its requests carry actor `agent` through `MOTH_ACTOR`, set by that settings file.
- User actions reach the agent through a `Stop` hook with `asyncRewake` running a new `moth wait`, which blocks on the feed from Task 1, bundles what the user did, prints it as `[moth] ...` notes and exits 2. A quiet switch in the window makes it stay silent.

Still to be tried there: a waiter armed for hours, what happens to half-typed text when a note arrives, scrollback, selection, paste and the mouse.

Not in stage 2 at all: a guess at what each result is and restart-proof paths (stage 3), toggles that hold a value (stage 4), the overlay (stage 5; the toolkit test found that an ordinary window does not show over Disco Elysium's fullscreen).
