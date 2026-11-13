# app toolkit test

Date: 2026-11-12. The throwaway test the design asks for at the start of stage 2 (section 8). The probes are not in the repository and are to be rewritten, not reused.

## what was decided

| question | answer | how it was checked |
| --- | --- | --- |
| interface toolkit | Qt 6 Widgets (`qtbase` 6.12.0 from Homebrew, LGPL, linked dynamically) | a 150-line window built and ran |
| terminal | libvterm 0.3.3 (MIT) for the screen state, painted by a Qt widget, over a pty from `forkpty` | Claude Code 2.1.323 ran in it |
| how a click reaches the agent | a `Stop` hook with `asyncRewake` that blocks until something happens, then exits 2 with the note on stderr | an idle session answered a note written from outside |
| per-session agent setup | `claude --settings <file>`: hooks in that file take effect, nothing global is edited | the hook above was loaded that way |
| overlay over the fullscreen game | not solved; see below | nine window levels tried |

## the terminal

- Colour (24-bit and 256), bold, italic, underline, reverse, box drawing, braille spinners, wide characters and colour emoji all drew correctly with Menlo 13 and Qt's own font fallback.
- Claude Code's full-screen interface drew and took typed input: the welcome screen, the prompt box, a reply, the status lines.
- Not tried: scrollback, selection and copy, paste, the mouse, an input method, resizing while the agent runs, how much CPU a naive repaint costs. Stage 2b has to cover them.
- The naive painter draws one cell at a time. It was fast enough to use; it is not how the real one should work.

## waking the agent

- Settings file passed with `--settings`: `{"hooks":{"Stop":[{"hooks":[{"type":"command","command":"<waiter>","asyncRewake":true,"timeout":400}]}]}}`.
- The waiter blocked on a file. 26 seconds after the agent's first reply, with the session idle at its prompt, a line was written to the file. The waiter printed it to stderr and exited 2. The agent showed `Stop hook feedback` and answered the note without anything being typed. The hook was armed again when that answer finished.
- What the user had typed into the prompt box and not sent was not tested.
- The hook's `timeout` is enforced, so a waiter that should stay armed for hours needs a long one. Not tested past 400 seconds.
- Channels (research preview, a confirmation screen at launch for a self-written one) were not tried: the hook works without either drawback.
- Launched from inside another Claude Code session, the inner one prints `Transcript saving is off`. That is the nesting, not the terminal.

## the overlay

- A borderless, non-activating `NSPanel` from another process, shown with `orderFrontRegardless` and able to join all spaces and fullscreen spaces, did not take focus from the game: the game stayed the frontmost app throughout.
- It was not visible over Disco Elysium while the game was frontmost and fullscreen, at any of these window levels: status bar (25), pop-up menu (101), overlay (102), screen saver (1000), assistive tech high (1500), the shielding level and one and two above it, and one below the maximum. Each panel reported itself as fully occluded, and a screenshot showed only the game.
- When another app is in front, the game is an ordinary window and other windows draw over it.
- So an ordinary Mac window cannot float over this game in its current fullscreen mode. This does not affect the choice of toolkit. It is the main open problem for stage 5; running the game windowed or borderless, and Wine's own fullscreen settings, are the first things to try there.

## why Qt

- Its text drawing handled everything Claude Code prints with no font work on moth's side.
- Tables with lazy loading, sockets, JSON, timers and a test library are all in `qtbase`.
- The alternatives were Dear ImGui over SDL3 (permissive and small, but text, input methods and a native feel would all be moth's job) and writing the window in Objective-C++ (which the language rule limits to thin glue).
