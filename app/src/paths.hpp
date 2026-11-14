#pragma once

#include <QString>
#include <QStringList>

// moth's home, where its socket and logs are: MOTH_HOME made absolute, else
// ~/Library/Application Support/moth (the rule the moth command uses)
QString mothHome();

// the moth command: MOTH_BIN, else moth next to the running program
QString mothBin();

// what the window's terminal runs first, in the user's shell: MOTH_TERMINAL, else the moth
// program's absolute path in single quotes and ` agent` (a login shell puts the folder of mothBin
// last on PATH, so the bare word `moth` could be another one). Set but empty: "", a plain shell
QString terminalCommand(const QString &mothBin);

// the user's shell: SHELL, else /bin/zsh
QString userShell();

// the environment of what runs in the terminal, as NAME=value: the app's own, with MOTH_HOME set
// to `home`, MOTH_BIN to `mothBin`, PATH with mothBin's folder first, SHELL to userShell(),
// TERM=xterm-256color, COLORTERM=truecolor, MOTH_BIN made absolute, TERM_PROGRAM=moth, and removed: MOTH_ACTOR (what the user types is theirs) and what belongs
// to the terminal or session that started the app (TERM_PROGRAM_VERSION, TERM_SESSION_ID, TMUX, TMUX_PANE,
// ITERM_SESSION_ID, CLAUDECODE, CLAUDE_CODE_ENTRYPOINT)
QStringList terminalEnv(const QString &home, const QString &mothBin);
