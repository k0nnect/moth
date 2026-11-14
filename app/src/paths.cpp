#include "paths.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>

QString mothHome() {
  const QString home = qEnvironmentVariable("MOTH_HOME");
  if (!home.isEmpty()) return QDir::isRelativePath(home) ? QDir::currentPath() + '/' + home : home;
  QString user = qEnvironmentVariable("HOME");
  if (user.isEmpty()) user = "/";
  return QDir(user).filePath("Library/Application Support/moth");
}

QString mothBin() {
  const QString bin = qEnvironmentVariable("MOTH_BIN");
  return bin.isEmpty() ? QCoreApplication::applicationDirPath() + "/moth" : bin;
}

QString terminalCommand(const QString &mothBin) {
  if (qEnvironmentVariableIsSet("MOTH_TERMINAL")) return qEnvironmentVariable("MOTH_TERMINAL");
  QString path = QFileInfo(mothBin).absoluteFilePath();
  path.replace('\'', "'\\''");
  return "'" + path + "' agent";
}

QString userShell() {
  const QString shell = qEnvironmentVariable("SHELL");
  return shell.isEmpty() ? "/bin/zsh" : shell;
}

QStringList terminalEnv(const QString &home, const QString &mothBin) {
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert("MOTH_HOME", home);
  const QFileInfo bin(mothBin);
  env.insert("MOTH_BIN", bin.absoluteFilePath());
  const QString path = env.value("PATH");
  const QString folder = bin.absolutePath();
  env.insert("PATH", path.isEmpty() ? folder : folder + ':' + path);
  env.insert("SHELL", userShell());
  env.insert("TERM", "xterm-256color");
  env.insert("COLORTERM", "truecolor");
  env.remove("MOTH_ACTOR");
  // what belongs to the terminal or the session that started the app is not this terminal's
  for (const char *name : {"TERM_PROGRAM_VERSION", "TERM_SESSION_ID", "TMUX", "TMUX_PANE", "ITERM_SESSION_ID", "CLAUDECODE",
                           "CLAUDE_CODE_ENTRYPOINT"})
    env.remove(name);
  env.insert("TERM_PROGRAM", "moth");
  return env.toStringList();
}
