#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QLocalServer>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <functional>

#include "board.hpp"
#include "core_client.hpp"

class QLocalSocket;

// A stand-in for moth's background service, for every app test: a QLocalServer at <home>/core.sock
// speaking the client protocol. Each request is answered from what `on` registered; with nothing
// registered, `status` is answered `not attached` (attached and hunt null) and any other command
// with the daemon's `unknown command` error. Like the daemon, a recorded request sends watchers a
// busy line when it arrives and its event (the next seq, its first line as summary) just before
// its reply, and a command that changes something is refused when sent with `record: false`.
// `watch` is answered with the newest seq and the connection kept for the feed; `after` is noted
// (see watches()) but nothing is replayed. Unlike the daemon it serves a request with no `proto`
// or another one; it notes the one each request and each watch carried (seen(), watchProtos()).
class FakeCore : public QObject {
public:
  explicit FakeCore(const QString &home);
  ~FakeCore() override;

  void on(const QString &cmd, std::function<Reply(const QStringList &args, const Opts &opts)> answer);
  void hold(const QString &cmd);                 // answer this command only when release() is called
  void release(const QString &cmd);
  void push(const QJsonObject &feedLine);        // to every watcher
  void dropWatchers();
  void stop(); void start();                     // the service going away and coming back
  struct Seen {
    QString actor, cmd;
    QStringList args;
    Opts opts;
    bool record;
    int proto = -1;  // the proto it carried; -1 when it sent none
  };
  QList<Seen> seen() const;                      // every request but watch, in order
  int watchers() const;
  QList<Opts> watches() const;                   // the options of every watch request, in order
  QList<int> watchProtos() const;                // the proto of every watch request, in order; -1: none
  // on (the default): a recorded request sends its own busy and event lines. off: only push() feeds,
  // for tests that set the order of an event and its reply themselves
  void feedRequests(bool on);
  // answer every `watch` with this error (as the daemon answers a 17th watcher `too many watchers`)
  // and close it; "" accepts watchers again
  void refuseWatch(const QString &error);
  // the game `name` closes: the next unrecorded `status` finds it gone, as the daemon's does. It is
  // answered {"attached":null,"hunt":null,"closed":name} with the text "not attached. <name>
  // closed.", and recorded all the same (R4): its event goes to watchers before the reply, with no
  // busy line before it. From then on `status` answers `not attached` (what `on` registered for
  // it is dropped)
  void closeGame(const QString &name);
  // from now on every ok `status` answer is sent as the test wrote it. Otherwise one that lacks
  // `proto` gets "proto": 2 and one that lacks `quiet` gets "quiet": false, as the daemon's all have
  void statusAsWritten(bool on = true);

private:
  struct Held { QPointer<QLocalSocket> socket; QJsonValue id; Seen request; };

  void accept();
  void heard(QLocalSocket *socket, const QByteArray &line);
  void answer(QLocalSocket *socket, const QJsonValue &id, const Seen &request);
  void send(QLocalSocket *socket, const QJsonObject &line);

  QString path_;
  QLocalServer server_;
  QHash<QString, std::function<Reply(const QStringList &, const Opts &)>> answers_;
  QSet<QString> holding_;
  QList<Held> held_;
  QList<QPointer<QLocalSocket>> connections_;
  QList<QPointer<QLocalSocket>> watchers_;
  QList<Seen> seen_;
  QList<Opts> watches_;
  QList<int> watchProtos_;
  bool feedRequests_ = true, asWritten_ = false;
  QString refuseWatch_;
  QString closing_;          // the game the next unrecorded status finds closed, or ""
  qint64 seq_ = 0;
};

// What most app tests start from: a fake core in a fresh home, a client on it and a board on that.
// Starting the service runs /usr/bin/true, so a stopped fake stays stopped.
struct Rig {
  // short: a socket path is limited to about 104 bytes
  explicit Rig(const QString &homeTemplate = "/tmp/moth-app-XXXXXX") : home(homeTemplate) {}
  QTemporaryDir home;
  FakeCore fake{home.path()};
  CoreClient core{home.path(), "/usr/bin/true"};
  Board board{&core};
  // waits until the board's feed is up and its first status read has been answered (the line
  // shows that read's first line)
  bool ready();
};

// a stand-in for the moth program, written into `dir`: each run adds the line
// "<its first argument> <its MOTH_HOME>" to dir/marker and does nothing else
struct FakeMoth {
  explicit FakeMoth(const QString &dir);
  QString path, marker;
  QStringList runs() const;  // one line per run, oldest first
};

// how many requests for `cmd` the fake has seen
int timesSeen(const FakeCore &fake, const QString &cmd);
