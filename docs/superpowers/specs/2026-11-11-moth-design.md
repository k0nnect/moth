# moth — design

Date: 2026-11-11
Status: approved by Chris on 2026-11-11
Project folder: `~/Documents/moth` (renamed from `deimos` on 2026-11-11 at Chris's request; empty apart from documents and brainstorm mockups)

Throughout this document, **"Chris decided"** marks something Chris said in the brainstorm. **"Claude's call"** marks a default Claude chose that Chris has not explicitly agreed to and may overrule. Anything under "Left to the builder" is the builder's to settle.

---

## 1. What moth is, and why

moth is a memory scanner and editor for single-player games: the job Cheat Engine does, rebuilt around an AI agent. Chris's words: "CE + agents", "a much better cheat engine", "a whole different upgrade".

It is a desktop app. The window has search, results and trainer panels on one side and a real terminal on the other, with a coding agent (Claude Code by default) already running in that terminal. The agent and the buttons share one session: every scan the user clicks is visible to the agent, the agent comments on it and suggests the next move, and anything the agent does shows up in the panels. While playing, a slim overlay floats over the game with toggles for the cheats moth has found.

Why it exists:

- Chris uses Cheat Engine on his own games and has it installed in most of his CrossOver bottles. He thinks a version built around agents would stand out.
- Cheat Engine's Mac support is weak. moth targets Chris's actual setup: an Apple Silicon Mac running Windows games through CrossOver.
- It is a portfolio project for his GitHub (`k0nnect`), written in OCaml and C++.

The name is Chris's choice. The first computer "bug" was a moth taped into the Harvard Mark II logbook in 1947.

## 2. Goals

All of these are **Chris decided** unless marked.

1. **Type-blind search.** The user types the number they see ("100"). moth searches every way that number could be stored. The user never picks a data type. Chris's complaint about Cheat Engine: "if u search for a value but its the wrong data type, you will never find the result you want." moth also adjusts searches on its own when a hunt is going nowhere.
2. **An agent built into the app.**
   - Claude loads automatically in moth's own terminal when the app opens.
   - The buttons and the agent are connected: every scan and click is handed to the agent automatically, with no need for the user to describe what they did.
   - The agent speaks up on its own with the next move.
   - The agent acts on the game (including writing values) without asking permission first.
   - AI is the default way to use moth, but typing to the agent is not the only way. Everything can also be done by clicking.
3. **Structure discovery.** After finding one value, moth maps what is around it (the player object: other stats nearby) and finds a path to the value that still works after the game restarts.
4. **Auto-built trainers.** moth turns what it finds into a trainer for that game, without the user writing anything.
5. **An overlay trainer.** A slim panel floating over the game with toggles, shown and hidden with a hotkey, so the user never leaves the game. (Chris picked this over a classic trainer window and a toggles-plus-values window.)
6. **Works very well on every single-player game in Chris's CrossOver bottles**, and on native Mac apps that are not locked down (see section 5 for what "locked" means).
7. **Private until done.** Nothing is published until all of the above are nailed. It is still built in an order (section 9).

### What "works very well" means for one game — Claude's call

A game passes when all of these hold on Chris's Mac:

- moth attaches without any macOS security setting being changed.
- A first full scan takes about 3 seconds per gigabyte of the game's writable memory or better. That is roughly 10 seconds for a game the size of Disco Elysium at its menu, and proportionally longer for bigger games. The bar scales with size; it is not a flat 10 seconds.
- At least three different values (for example health, money, a skill stat) are found by typing only the numbers shown on screen, never a data type.
- A trainer built from those values still works after the game is quit and relaunched.
- The overlay shows over the game and its toggles work during play.

## 3. Non-goals for this release

- **Multiplayer and anti-cheat.** moth is for single-player and offline play. It contains no anti-cheat or anti-tamper bypass. (Claude's call, stated to Chris, not objected to.) How far that is enforced is Claude's call:
  - moth does not try to detect online modes in general. That is not reliably possible.
  - If a well-known anti-cheat component is present in or alongside the target, moth refuses to attach and says why.
  - The instructions moth gives the agent say single-player and offline only, and tell it to decline online use.
- **Cheat Engine's code-level features.** Disassembler, assembly editing, code-injection scripts, speedhack, Lua scripting. The aim is to beat Cheat Engine outright at the main loop (find a value, lock it in, turn it into a trainer), not to match twenty years of features. (Claude's call. Chris said he wants moth "better than CE in every way"; Claude told him the first release beats it at the main loop and grows from there, and Chris then defined "nailed" as the goals above.)
- **Native Windows or Linux releases.** The in-bottle helper should make these possible later. They are not required now.
- **The Sims 4 and other locked Mac apps** are a stretch goal, not part of the release bar. (Chris agreed.)

## 4. Anti-goals

These would make moth a failure even if it technically worked.

- **moth being "just a Claude chat".** The product is the app. It must not be an MCP server or plugin where typing to Claude is the only way in. (Chris decided.)
- **The user ever having to choose a data type.** (Chris decided.)
- **Anything published before the release bar is met.** (Chris decided.)
- **Silent failure.** A locked app, a blocked game or a stale trainer must be reported plainly. (Claude's call.)
- **Writing to the wrong place.** After a game update, a trainer must not write to addresses it cannot re-verify. (Claude's call.)

## 5. Constraints and tested facts

These were measured on Chris's Mac on 2026-11-11 with throwaway probes. The probes are not part of the project and should be rewritten, not reused.

### Machine

- 14-inch MacBook Pro, Apple M5, 24 GB, macOS 27.0.1, arm64.
- System Integrity Protection is enabled and must stay enabled. moth must not require disabling it.
- No passwordless `sudo`. moth must not need root for normal use.
- Toolchain present: Apple clang 21, Homebrew, `mingw-w64` 14.0.0 (`x86_64-w64-mingw32-gcc`/`g++`), `opam`. **Not present:** `ocaml` and `dune` are not on the PATH; an OCaml switch has to be set up.

### CrossOver

- CrossOver 26.3 at `~/Applications/CrossOver.app`. Command-line tools at `Contents/SharedSupport/CrossOver/bin/` (`wine`, `wineserver`, `cxstart`, `cxbottle`, ...).
- Bottles at `~/Library/Application Support/CrossOver/Bottles/`: `Disco Elysium`, `Mafia The Old Country`, `nfs`, `opsec`, `Paralives`, `Sleeping Dogs`, `Sons of the Forest`, `Steam`, `uhmm`.
- Games are installed outside the bottles, under `~/Documents/`, and reached through each bottle's `Y:` drive (mapped to the home folder). Found so far:
  - `~/Documents/Disco Elysium/Disco Elysium/disco.exe` (Unity, 64-bit)
  - `~/Documents/Sleeping Dogs - Definitive Edition/sdhdship.exe`
  - `~/Documents/Sons of the Forest/Sons Of The Forest/SonsOfTheForest.exe`
  - `~/Documents/Paralives/Paralives.exe`
  - `~/Documents/Mafia The Old Country/MafiaTheOldCountry/Binaries/Win64/MafiaTheOldCountry.exe`
  - `~/Documents/Mafia III Definitive Edition/Mafia3DefinitiveEdition.exe`
- CrossOver shortcuts also exist for Need for Speed Heat, Grey Hack, Hackhub and "UTW_Gothic". Their install folders were not located. Need for Speed Heat and Grey Hack have online modes.
- Cheat Engine is installed in most bottles, and there is a Disco Elysium cheat table at `~/Documents/Disco Elysium (version 2024-06-24).CT`. Both are useful for head-to-head comparison.

### Getting into a CrossOver game

- **From the macOS side: blocked.** CrossOver's Wine loader is an x86_64 binary (runs under Rosetta) signed with the hardened runtime and without `get-task-allow`. `task_for_pid` on a Wine process fails (`KERN_FAILURE`, 5) even from a binary signed with the `com.apple.security.cs.debugger` entitlement. Getting past this would need SIP off or CrossOver re-signed. Both are ruled out.
- **From inside the bottle: works.** A Windows program started in the same bottle can open the game with ordinary Windows calls (`CreateToolhelp32Snapshot`, `OpenProcess(PROCESS_ALL_ACCESS)`, `VirtualQueryEx`, `ReadProcessMemory`, `WriteProcessMemory`). No macOS setting was changed.
  - Launch form that worked: `"<CrossOver>/bin/wine" --bottle "<bottle>" --workdir "<dir>" "<absolute macOS path to .exe>"`. A bare relative `.exe` name failed with "cannot execute".
  - Each bottle is its own Windows world. The helper must run in the same bottle as the game.
  - CrossOver prints its own lines (for example `msync: up and running.`) to the launched program's output streams.
- **Measured on Disco Elysium, sitting at its main menu:**
  - 3,357 MB of committed writable memory in 532 regions; none unreadable.
  - Reading all of it and comparing every aligned 4-byte slot: 4.5 s on the first pass, 3.6 s on the second (about 750 to 930 MB/s), reading in 64 MB chunks.
  - Aligned 4-byte matches for 100: 241,178 as a 32-bit integer, 18,382 as a 32-bit float.
  - Writing a value back unchanged succeeded.
- Not yet tested: any other game, a game mid-play, any game with tamper protection, 32-bit games.

### Getting into a native Mac app

- **Not locked down:** a binary that is ad-hoc signed with `com.apple.security.cs.debugger` could `task_for_pid` a same-user, non-hardened process and read and write its memory. No prompt appeared. (Chris's account is in the `_developer` group.) An unsigned-entitlement probe was refused.
- **Locked down (hardened runtime, no `get-task-allow`):** refused. This covers The Sims 4 and Lunar Client on Chris's Mac.
- A possible workaround for locked apps is re-signing a copy of the app so it allows debugging. It is untested and can break apps tied to store copy protection. This is the stretch goal.
- Chris barely runs native Mac games. The only real native target on his machine is The Sims 4.

### Project constraints

- Languages: OCaml and C++. (Chris decided.)
  - A small amount of Objective-C++ is allowed for Mac-only window behaviour, such as keeping the overlay above a fullscreen game without taking focus and global hotkeys. (Chris decided, 2026-11-11.) Keep it to thin glue files. The rest of the app stays plain C++.
- Public code eventually goes out under `k0nnect`, in Chris's lowercase style. The public face (README, identity) follows his naming and outlining playbook: editorial, all lowercase, no emojis or gradients, identity rooted in the name's real history. That work happens at release time, not now.

## 6. The product

### 6.1 The app window

Layout is **Claude's call**: Chris was shown three layouts and did not pick one, so Claude chose "terminal down the right side" because the agent is the default way in and talks a lot.

- **Left side, top to bottom:**
  - Which game moth is attached to, and a way to pick or launch one.
  - A search box with no type selector, plus narrowing controls: went down, went up, same, changed.
  - The results list. Each result shows its current value, a plain-words type tag (whole number, decimal), and moth's guess at what it is. A "keep" action turns a result into a cheat.
  - The trainer panel: the cheats kept so far, each with a toggle and its live value. Values can be typed over. (Editable live values are Claude's call.)
- **Right side:** the built-in terminal, running the agent.
- **A quiet switch** that stops the agent commenting on every action. (Claude's call.)

Mockups from the brainstorm are in `.superpowers/brainstorm/94753-1794381213/content/` in the project folder: `trainer-layouts.html` (Chris chose C, the overlay), `hunt-layouts.html`, and `app-with-terminal.html` (B is the layout described here). They show layout only. Styling was deliberately rough.

### 6.2 The built-in terminal and the agent

- The terminal is a real terminal, in moth's own window, not a chat box. (Chris decided: "its own unique terminal window".)
- When moth opens, it starts Claude Code in that terminal automatically. (Chris decided.)
- **Claude's calls:**
  - It is a normal shell session underneath, so plain moth commands and other agents (Codex, OpenCode) also work there. Which agent starts by default is a setting. Claude Code is the default and the only one that must work for release.
  - moth ships whatever an agent needs to use it (tool definitions and instructions), wired in automatically for the session. The user explains nothing.
  - moth's own actions are pre-approved for the agent in that session, so "acts without asking" holds. Anything else the agent does in the shell follows the user's normal agent settings.
  - Nobody needs to talk to the agent. All panel controls work if the agent is quiet, absent or not installed.

### 6.3 The shared session

This is the heart of the product and the thing Chris worked hardest to describe.

- There is one hunt in progress, and both the user and the agent have their hands on it.
- **User to agent:** every panel action (scan, narrow, keep, toggle, edit) is recorded with its outcome and handed to the agent automatically. Example: the user scans for 100 and gets 241,178 matches, takes damage and clicks "went down" leaving 3, then types only "which one is it?". The agent can answer because it saw all of it.
- **Agent speaks up:** after a user action, the agent offers the next move unprompted ("that's a lot of matches, go take damage and hit went down"). (Chris decided.)
- **Agent to user:** when the agent runs a scan, keeps a value or flips a toggle, the panels update as if the user had clicked.
- **Claude's calls:**
  - Rapid actions are bundled so the agent is not woken for every click.
  - The quiet switch stops the unprompted commentary. Actions are still recorded, and the agent can still look them up when asked.
  - Every write to game memory is recorded in the session, with the previous value, and the last write can be undone.

### 6.4 Type-blind search

- **Input:** the number the user sees, as typed ("100", "132.50").
- **What gets searched, all at once:** whole numbers of every size that can hold the value, and decimals of both sizes. Decimal matching allows for display rounding (a health bar showing 85 may hold 85.3).
- **Narrowing:** the user gives the new number, or says went up, went down, same or changed. Interpretations that stop matching drop out on their own.
- **Auto-adjust** (Chris decided the behaviour, Claude sketched the means): when a hunt is dying, moth widens it without being asked, rather than returning nothing. Things worth trying include unaligned positions and values stored scaled (times 10, times 100). The cheap wide interpretations should be carried from the first scan, since they cannot be recovered later without the old memory contents.
- **No number to type** (a bar with no figure): moth can hunt by change alone, starting from a snapshot. (Claude's call that this is in scope. Cheat Engine has it, and moth must not be worse.)
- **Honest dead end:** if every interpretation dies, moth says so and offers the change-based hunt.
- **Speed target** (Claude's call, and tighter than the pass bar in section 2): first scan of a roughly 3.5 GB game in about 5 seconds, later narrowing passes in about a second or less for under a million survivors. The naive probe already hit 3.6 to 4.5 seconds for a first pass.

### 6.5 Structure discovery

- **Restart-proof paths.** For a kept value, moth finds a route from something fixed in the game (a loaded module plus offsets) to the value, so it can be found again next launch. A path only counts as verified after the game has actually been restarted and the path still lands on the right value.
- **The neighbourhood.** moth reads the object around the value, works out what each slot looks like (pointer, decimal, whole number, text), watches which ones change as the user plays, and proposes names. The agent helps with naming.
- moth can quit and relaunch a game in its bottle itself, since verifying paths needs restarts.

### 6.6 Trainers

- One trainer per game, saved as a file moth runs. It is not a separate app per game. (Claude's call.)
- Built automatically: every kept, verified value becomes a toggle. The trainer is the accumulated set.
- Each cheat records: a name, how to find the value again, its type, what to do (hold at a value, set once), an optional hotkey, and when and on which game version it was verified.
- **Version check** (Claude's call): a trainer records a fingerprint of the game build. On load, moth re-finds each value and checks it looks sane. If the game has changed and a value cannot be re-verified, that cheat is marked stale and is not written to. The agent then offers to re-find it.
- Freezing means the value is rewritten on a short interval for as long as the toggle is on.

### 6.7 The overlay

- A slim panel listing the current trainer's toggles, floating above the game. (Chris decided, option C.)
- One hotkey shows and hides it. Cheats can have their own hotkeys.
- It must not steal focus from the game.
- **Claude's calls:**
  - It is an ordinary Mac window kept on top. Nothing is injected into the game to draw it.
  - Hunting happens in the main window; the overlay is for playing with a finished trainer.
  - A one-time macOS permission prompt for global hotkeys is acceptable if the system requires it.
- Whether the panel shows over a fullscreen CrossOver game is **untested** and is the main risk for this piece.

## 7. How it gets made

The language choice is Chris's. Everything else in this section is **Claude's call**, shaped by the tests in section 5.

### Three parts

1. **The helper (C++).** A small program that sits next to the game and does the memory work: listing processes and memory regions, bulk reading, running the scan and narrowing comparisons, reading and writing values, holding frozen values, and the raw groundwork for path finding. The comparisons run in the helper so gigabytes never leave it.
   - For CrossOver games it is a Windows program, cross-compiled with mingw-w64 and launched inside the game's bottle.
   - For unlocked native Mac apps it is the same engine with a macOS back end, signed with the debugger entitlement.
   - A 64-bit Windows helper is expected to handle 32-bit games too; this needs confirming.
2. **The core (OCaml).** The brain, running natively on the Mac. It owns the session: what is being hunted, the history of scans, kept values, structure maps and trainers. It decides which interpretations to search and when to widen, scores and labels candidates, infers structure, solves restart-proof paths from what the helper gathers, builds and checks trainers, and records every action. The app, the command line and the agent all talk to the core, which is what makes the session shared.
3. **The app (C++).** The window, the panels, the built-in terminal and the overlay.

The split is meant to be honest: C++ where the work touches raw memory, the operating system or the screen; OCaml where the work is reasoning over structured data.

### What moth may do to a game

- **Allowed:** reading and writing the game's memory from outside, which is everything the tests in section 5 did.
- **Allowed if it proves reliable:** watching the game from outside as a debugger to learn what reads or writes an address. This would make stage 3 much easier, but whether it works for a Windows game under CrossOver on Apple Silicon is unknown and needs its own test. Stage 3 must be planned so that it works without it: paths found from data alone are the baseline, and debugger-style watching is an accelerator if the test passes.
- **Not in this release:** loading moth's code into the game, allocating memory inside it, or hooking its code. This keeps moth's footprint in the game small, lowers the chance of crashes and of tripping tamper protection, and matches the overlay rule that nothing is injected. It also means in-game scanning is not available as a speed fix. If out-of-process reads turn out too slow for a large game, that game is reported against the bar and the question goes back to Chris.

### Decisions the tests already forced

- The helper for CrossOver games must be a Windows program inside the bottle. Direct access from macOS is not available.
- The helper and the core are separate processes in different worlds (one inside Wine, one native), so they need a channel between them. A local network connection on the loopback address is the obvious candidate. Standard output is a poor one, because CrossOver writes its own lines into it.

## 8. Left to the builder

- The message format between helper and core, and between core and its clients.
- The C++ interface toolkit. It must provide: a real terminal good enough to run Claude Code's full-screen interface (colour, cursor addressing, mouse, Unicode), and an always-on-top panel that does not take focus. Settle this with a short throwaway test at the start of the app stage, including the overlay-over-fullscreen-game check, since that may decide the toolkit. Limits on the choice (Claude's calls):
  - Libraries moth depends on may be written in any language. The language constraint is about moth's own source.
  - Avoid dependencies under the GPL, so the licence Chris picks at release stays open. Permissive licences and dynamically linked LGPL libraries are fine.
  - Working on Windows or Linux later is a bonus, not a requirement.
- How user actions reach the agent so that it speaks up. Any mechanism is fine if the behaviour in 6.3 holds with Claude Code. A documentation check on 2026-11-11 found these candidates; re-check them before building, since two are previews or were not fully confirmed:
  - **Channels** (`code.claude.com/docs/en/channels`): a small tool server that pushes a message into the open session, which starts a turn when the agent is idle and queues when it is busy. Research preview. A custom one needs a launch flag that shows a confirmation screen on every start, and it requires a claude.ai or Console login. moth must not dismiss that confirmation on the user's behalf.
  - **A background hook that wakes the session** (`code.claude.com/docs/en/hooks-guide`, the `asyncRewake` option). No preview gate, but its exact configuration was not confirmed.
  - **Typing a short note into the terminal.** moth owns the terminal, so it can always enter a visible one-line note such as "[moth] you scanned for 100: 241,178 whole, 18,382 decimal". This works with any agent and is the guaranteed fallback. It must not collide with something the user is in the middle of typing.
  - Not usable for this: Remote Control, headless streaming input, and resuming the session from another process. None of them attach to a session already running in a terminal.
  - Order of preference (Claude's call): whichever of the first two works without friction on every launch, otherwise the typed note. The real terminal is never traded away to get this. If none of the three gives acceptable behaviour, the agent catches up the next time the user types, and the gap goes back to Chris.
- Pre-approving moth's own tools and giving the agent moth's instructions should be done per session (launch flags or a moth-owned config file), never by editing the user's global agent settings.
- How moth's tools and instructions are presented to the agent (tool server, command-line commands, instruction files).
- The exact set of interpretations, tolerances and widening order for type-blind search, and how candidates are stored.
- Where a full memory snapshot for change-based hunts lives. It can be several gigabytes; the machine has 24 GB of memory.
- The path-finding method, its depth and offset limits, and any engine-specific shortcuts (the test games span Unity and Unreal, plus at least two in-house engines).
- Scoring and naming heuristics.
- Trainer file format and where trainers are stored.
- How moth works out which bottle a running game belongs to.
- Freeze interval, hotkey handling, settings storage.
- Repository layout and build tooling.

## 9. Build order

Chris decided the rule: build in an order, publish nothing until everything is nailed. The order itself is Claude's call, shown to Chris and accepted.

Each stage gets its own detailed plan, written when the stage starts. Each stage ends by being proven on a real game.

1. **The engine.** The in-bottle helper, the core's type-blind search, and plain commands to drive them. Done when, on Disco Elysium, values can be found and changed by typing only the numbers on screen.
2. **The app.** Window, panels, built-in terminal with the agent auto-loaded, and the shared session where the agent watches and speaks up. Starts with the toolkit test from section 8.
3. **Structure discovery.** The neighbourhood map and restart-proof paths, verified across real restarts.
4. **Auto-built trainers.** Saving, loading, version checks, stale handling.
5. **The overlay.**
6. **Prove it everywhere.** Every single-player game in Chris's bottles against the bar in section 2, plus the native Mac side:
   - The helper's macOS back end is built in this stage. The scan engine is written from stage 1 so that only the operating-system layer differs between the two back ends.
   - Chris has no unlocked native Mac game, so the native side is proven on a native stand-in test program with known values and on one real unlocked app from his Mac (S4Studio is unlocked) for attach, scan and read. It stays in the release bar on that proof.
   - Locked apps are reported as locked.
   - The Sims 4 unlock is attempted as a stretch.

Only after stage 6: the public face, the licence choice, and publishing.

**Changed by Chris on 2026-11-13: stage 6 is dropped.** His words: "can we skip stage 6 i really dont care about it". What follows from that (Claude's reading, his to correct):

- The build order ends at stage 5. "Nothing public until everything is nailed" now means stages 1 to 5.
- Goal 6 (every single-player game in his bottles, and unlocked native Mac apps) is no longer part of the release bar. moth is proven on the games each stage is tested on, which so far is Disco Elysium.
- The helper's macOS back end, the native stand-in program and the Sims 4 stretch were all stage 6 work and are not built.
- Section 12's open question about the four games whose folders were not found no longer needs an answer.

## 10. How it gets verified

- **A stand-in target.** A small Windows test program with known values in every form moth claims to handle (each number size, decimals, scaled values, unaligned values, values behind chains of pointers, values that move between runs). It runs in a scratch bottle (`uhmm` was used for the probes) so the engine can be tested automatically without a real game.
- **Unit tests** for the core's reasoning: interpretation planning, narrowing, widening, path solving, trainer checks.
- **Real-game checks** need Chris at the keyboard (taking damage, spending money). Each game gets a dated entry in a verification log: game, build, what was found, whether it survived a restart, anything that failed. Work that has not been checked on a real game is labelled as such, the same way Chris's earlier project labelled anything not verified on hardware.
- **Head to head.** For each stage-1 hunt, note what the same hunt takes in Cheat Engine, since "better than Cheat Engine" is the claim.

## 11. Known risks

1. **The interface toolkit.** A C++ toolkit with a terminal good enough for Claude Code and an overlay that shows over a fullscreen CrossOver game is unproven. Objective-C++ glue for the Mac-only parts is allowed, which lowers this risk. If the app as a whole cannot be done in C++ with that glue, it conflicts with the language constraint and goes back to Chris.
2. **Making the agent speak up.** Three mechanisms exist on paper (section 8) and one of them, typing a note into the terminal, cannot be blocked. None has been tried with moth, and how natural each feels is unknown.
3. **Restart-proof paths.** These are the hardest technical piece. Path searches over 3 GB or more can be slow and produce huge candidate sets, and engines differ.
4. **Untested games.** Five of the six located games have never been attached to. Memory mid-play will be larger than at a menu. A game with tamper protection may refuse reads. moth does not bypass that, which would conflict with "works on every game in my bottles" and goes back to Chris if it happens.
5. **Global hotkeys while a game has focus.**
6. **Change-based hunts** need a multi-gigabyte snapshot.
7. **Size.** This is a large project. Stage 1 is small and proves the foundation; stages 2 and 3 carry most of the risk.

## 12. Open items that are Chris's, not blocking stage 1

- Licence, decided at release.
- The four games whose install folders were not located (Need for Speed Heat, Grey Hack, Hackhub, UTW_Gothic): whether they count toward the release bar. Grey Hack is online-only as far as Claude knows and would be excluded.
