#pragma once

#include <QString>
#include <QStringList>

// moth's home, where its socket and logs are: MOTH_HOME made absolute, else
// ~/Library/Application Support/moth (the rule the moth command uses)
QString mothHome();

// the moth command: MOTH_BIN, else moth next to the running program
QString mothBin();

// what the window's terminal runs first, in the user's shell: MOTH_TERMINAL, else `moth agent`.
// Set but empty: "", a plain shell
QString terminalCommand();

// the user's shell: SHELL, else /bin/zsh
QString userShell();

// the environment of what runs in the terminal, as NAME=value: the app's own, with MOTH_HOME set
// to `home`, MOTH_BIN to `mothBin`, PATH with mothBin's folder first, SHELL to userShell(),
// TERM=xterm-256color, COLORTERM=truecolor, and MOTH_ACTOR removed (what the user types is theirs)
QStringList terminalEnv(const QString &home, const QString &mothBin);
