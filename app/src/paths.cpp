#include "paths.hpp"

#include <QCoreApplication>
#include <QDir>

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
