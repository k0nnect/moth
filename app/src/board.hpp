#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <optional>

#include "core_client.hpp"

struct Attached {
  int pid; QString name; int bits; QString bottle;
  bool operator==(const Attached &) const = default;
};
struct Hunt {
  qint64 main; qint64 wide; QList<QPair<QString, qint64>> byTag; bool blind; bool canCompare;
  bool operator==(const Hunt &) const = default;
};

// What the window knows of moth, and the one way its panels ask for anything. At its start it
// starts the service if nothing answers (the one time the app does so by itself), then reads
// `status`. It follows the core's feed (started here) and reads `status` after every event, when
// the feed comes back, and on every tick.
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
  // a recorded request as the user. Refused while working: line becomes "moth is busy: <what>",
  // nothing is sent and `then` is not called. A failed request's whole error becomes the line, and
  // its event (whose summary is the error's first line) leaves it so. With no service answering,
  // it starts one.
  void run(const QString &cmd, const QStringList &args = {}, const Opts &opts = {},
           std::function<void(const Reply &)> then = {});
  // an unrecorded read. Dropped (and `done` never called) while working, while the service is
  // being started at the board's start, or when a read with the same command, arguments and
  // options is still out. It never starts the service. Returns true when the read was sent or an
  // identical one is out (a reply will come), false when it was dropped for working or starting
  // (no reply will come).
  bool read(const QString &cmd, const QStringList &args, const Opts &opts,
            std::function<void(const Reply &)> done);
  void tick();               // the once-a-second poll; the window calls it from a timer while visible
signals:
  void changed();            // anything the getters return changed
  // a recorded request ended (anyone's): lists read themselves again. Once per event, after the
  // `status` read sent after that event has come back, so `attached()` and `hunt()` are as it left them
  void turned();
  void ticked();             // a poll: lists refresh the values they show

private:
  void began(const QJsonObject &what);
  void ended(const QJsonObject &ev);
  void feedWent(bool up);
  void refused(const QString &error);
  void ask(const QString &cmd, const QStringList &args, const Opts &opts, std::function<void(const Reply &)> done);
  void readStatus();
  void tookStatus(const Reply &r);
  void showStatus(const Reply &r);
  void refresh();
  void turn();

  CoreClient *core_;
  std::optional<Attached> attached_;
  std::optional<Hunt> hunt_;
  QString counts_, what_, line_;
  bool working_ = false, connected_ = false;
  bool lost_ = false;          // the feed went down and has not come back
  bool starting_ = true;       // the start of the service at the board's start is under way
  bool needStatus_ = true;     // no status read has come back since the start or since the feed was lost
  bool owesLine_ = true;       // ... and nothing newer has been said: that status read is said
  bool statusAgain_ = false;   // an event came while a status read was out: read again when it returns
  int turnsOwed_ = 0;          // events whose `turned` waits for the status read after them
  QString refusal_;            // the core's refusal of the feed, while the feed is not up
  QSet<QString> reading_;      // the reads still out
};
