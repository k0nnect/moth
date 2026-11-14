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
  QString line() const;      // the newest thing to tell the user; the stale sentence while stale()
  bool quiet() const;        // the last status read said the agent is not told what the user does
  bool stale() const;        // the last status read had a `proto` other than 2, or none: the service is from another build
  void say(const QString &line);
  QString home() const;      // moth's home, as the client uses it
  // why a request would not be sent now: "moth is busy: <words>" while one of the user's own
  // requests is unanswered (its words: "undo", "find 100") or while working ("moth is busy: " +
  // what()); "moth is starting…" while the service is being started at the board's start; ""
  // when nothing stands in the way
  QString busyText() const;
  // a recorded request as the user; true when it was sent. Refused while one of the user's own
  // requests is still unanswered, or while working: the line becomes busyText(), nothing is sent,
  // `then` is not called, and it returns false. The core's whole answer becomes the line, all its
  // lines: the text when it worked, the error when it failed; its event (whose summary is the
  // answer's first line) leaves it so. With no service answering, it starts one.
  bool run(const QString &cmd, const QStringList &args = {}, const Opts &opts = {},
           std::function<void(const Reply &)> then = {});
  // an unrecorded read. Dropped while working or while the service is being started at the
  // board's start: it returns false, and `done` is never called. When a read with the same
  // command, arguments and options is still out, nothing more is sent and it returns true, but
  // this `done` is never called either: only the first caller's is, once its reply comes.
  // Otherwise the read is sent and it returns true; `done` is called once with its reply. It never
  // starts the service.
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
  QString running_;            // the words of the user's own request still unanswered, or ""
  bool working_ = false, connected_ = false, quiet_ = false, stale_ = false;
  bool lost_ = false;          // the feed went down and has not come back
  bool stopped_ = false;       // the last event was a stop: the feed is about to drop
  bool starting_ = true;       // the start of the service at the board's start is under way
  bool needStatus_ = true;     // no status read has come back since the start or since the feed was lost
  bool owesLine_ = true;       // ... and nothing newer has been said: that status read is said
  bool statusAgain_ = false;   // an event came while a status read was out: read again when it returns
  int turnsOwed_ = 0;          // events whose `turned` waits for the status read after them
  QString refusal_;            // the core's refusal of the feed, while the feed is not up
  QSet<QString> reading_;      // the reads still out
};
