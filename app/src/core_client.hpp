#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

class QLocalSocket;
class QProcess;

// a request's options, as the command line makes them: `--all` is {"all","true"}, `-n 200` {"n","200"}
using Opts = QList<QPair<QString, QString>>;
struct Reply { bool ok = false; QJsonValue result; QString text; QString error; };

// moth's background service, reached at <home>/core.sock: one connection per request, and one
// more that follows the feed. Nothing here blocks or uses a thread.
class CoreClient : public QObject {
  Q_OBJECT
public:
  CoreClient(QString home, QString mothBin, QObject *parent = nullptr);
  // one request on its own connection, actor "user"; `done` runs once, on the caller's thread, and
  // never before call() returns. When no service answers, a recorded request (the user's own
  // action) runs `mothBin status` once to start one and is tried again when that ends (10 seconds
  // at most). An unrecorded one (a poll) never starts it: it fails at once.
  void call(const QString &cmd, const QStringList &args, const Opts &opts, bool record,
            std::function<void(const Reply &)> done);
  // the app's start of the service, once, at launch: when nothing answers at the socket, runs
  // `mothBin status` (10 seconds at most); a running service is asked nothing. Then `then`, on the
  // caller's thread, never before this returns.
  void startService(std::function<void()> then);
  void startFeed();  // `watch`; when it drops, tries again every second with `after` = the last seq seen
signals:
  void busy(const QJsonObject &what);   // a recorded request started
  // the signal's name hides QObject::event(QEvent *), which Qt still calls through QObject
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Woverloaded-virtual"
  void event(const QJsonObject &ev);    // a recorded request ended
#pragma clang diagnostic pop
  void feedUp(bool up);

private:
  struct Exchange;
  void attempt(const std::shared_ptr<Exchange> &x);
  void runMoth(std::function<void()> then);
  void watch();
  void heard(QLocalSocket *feed, bool &answered);
  void dropFeed(QLocalSocket *feed);
  void setFeedUp(bool up);
  QString socketPath() const;

  QString home_, mothBin_;
  QProcess *starting_ = nullptr;               // `moth status`, while it runs
  QList<std::function<void()>> waiting_;       // what goes on once it has ended
  bool feedStarted_ = false, feedUp_ = false;
  qint64 lastSeq_ = -1;                         // the newest event seen; -1 before the first watch
};
