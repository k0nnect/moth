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

// What the window knows of moth, and the one way its panels ask for anything. It follows the
// core's feed (started here) and reads `status` after every event, after the feed comes back, and
// on every tick.
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
  // nothing is sent and `then` is not called. A failed request's error becomes the line.
  void run(const QString &cmd, const QStringList &args = {}, const Opts &opts = {},
           std::function<void(const Reply &)> then = {});
  // an unrecorded read. Dropped (and `done` never called) while working, or when a read with the
  // same command, arguments and options is still out
  void read(const QString &cmd, const QStringList &args, const Opts &opts,
            std::function<void(const Reply &)> done);
  void tick();               // the once-a-second poll; the window calls it from a timer while visible
signals:
  void changed();            // anything the getters return changed
  void turned();             // a recorded request ended (anyone's): lists read themselves again
  void ticked();             // a poll: lists refresh the values they show

private:
  void began(const QJsonObject &what);
  void ended(const QJsonObject &ev);
  void feedWent(bool up);
  void readStatus();
  void tookStatus(const Reply &r);

  CoreClient *core_;
  std::optional<Attached> attached_;
  std::optional<Hunt> hunt_;
  QString counts_, what_, line_;
  bool working_ = false, connected_ = false;
  bool owesStatusLine_ = true;  // at the start and after the feed was lost: the next status read is said
  QSet<QString> reading_;       // the reads still out
};
