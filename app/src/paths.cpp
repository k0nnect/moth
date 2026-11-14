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

QString terminalCommand() {
  return qEnvironmentVariableIsSet("MOTH_TERMINAL") ? qEnvironmentVariable("MOTH_TERMINAL") : "moth agent";
}

QString userShell() {
  const QString shell = qEnvironmentVariable("SHELL");
  return shell.isEmpty() ? "/bin/zsh" : shell;
}

QStringList terminalEnv(const QString &home, const QString &mothBin) {
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert("MOTH_HOME", home);
  env.insert("MOTH_BIN", mothBin);
  const QString path = env.value("PATH");
  const QString folder = QFileInfo(mothBin).absolutePath();
  env.insert("PATH", path.isEmpty() ? folder : folder + ':' + path);
  env.insert("SHELL", userShell());
  env.insert("TERM", "xterm-256color");
  env.insert("COLORTERM", "truecolor");
  env.remove("MOTH_ACTOR");
  return env.toStringList();
}
