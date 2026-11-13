#include "board.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>

namespace {

const QString lostLine = QString::fromUtf8("lost moth's background service. trying again…");

std::optional<Attached> attachedOf(const QJsonValue &v) {
  if (!v.isObject()) return std::nullopt;
  const QJsonObject o = v.toObject();
  return Attached{o.value("pid").toInt(), o.value("name").toString(), o.value("bits").toInt(), o.value("bottle").toString()};
}

std::optional<Hunt> huntOf(const QJsonValue &v) {
  if (!v.isObject()) return std::nullopt;
  const QJsonObject o = v.toObject();
  QList<QPair<QString, qint64>> byTag;
  for (const QJsonValue &t : o.value("by_tag").toArray()) {
    const QJsonArray pair = t.toArray();
    byTag.append({pair.at(0).toString(), pair.at(1).toInteger()});
  }
  return Hunt{o.value("main").toInteger(), o.value("wide").toInteger(), byTag, o.value("blind").toBool(),
              o.value("can_compare").toBool()};
}

// what identifies a read, so the same one is not sent twice
QString keyOf(const QString &cmd, const QStringList &args, const Opts &opts) {
  QJsonArray o;
  for (const auto &[k, v] : opts) o.append(QJsonArray{k, v});
  return QString::fromUtf8(QJsonDocument(QJsonArray{cmd, QJsonArray::fromStringList(args), o}).toJson(QJsonDocument::Compact));
}

QString statusKey() { return keyOf("status", {}, {}); }

}  // namespace

Board::Board(CoreClient *core, QObject *parent) : QObject(parent), core_(core) {
  connect(core, &CoreClient::busy, this, &Board::began);
  connect(core, &CoreClient::event, this, &Board::ended);
  connect(core, &CoreClient::feedUp, this, &Board::feedWent);
  core->startFeed();
  // the one time the app starts the service by itself; reads wait for it, then status is read
  core->startService([self = QPointer<Board>(this)] {
    if (!self) return;
    self->starting_ = false;
    self->readStatus();
  });
}

const std::optional<Attached> &Board::attached() const { return attached_; }
const std::optional<Hunt> &Board::hunt() const { return hunt_; }
QString Board::counts() const { return counts_; }
bool Board::working() const { return working_; }
QString Board::what() const { return what_; }
bool Board::connected() const { return connected_; }
QString Board::line() const { return line_; }

void Board::say(const QString &line) {
  owesLine_ = false;
  if (line == line_) return;
  line_ = line;
  emit changed();
}

void Board::run(const QString &cmd, const QStringList &args, const Opts &opts, std::function<void(const Reply &)> then) {
  if (working_) {
    say("moth is busy: " + what_);
    return;
  }
  core_->call(cmd, args, opts, true, [self = QPointer<Board>(this), then](const Reply &r) {
    if (!self) return;
    if (!r.ok) self->say(r.error);
    if (then) then(r);
  });
}

void Board::read(const QString &cmd, const QStringList &args, const Opts &opts, std::function<void(const Reply &)> done) {
  if (working_ || starting_) return;
  const QString key = keyOf(cmd, args, opts);
  if (reading_.contains(key)) return;
  reading_.insert(key);
  core_->call(cmd, args, opts, false, [self = QPointer<Board>(this), key, done](const Reply &r) {
    if (!self) return;
    self->reading_.remove(key);
    if (done) done(r);
  });
}

void Board::tick() {
  readStatus();
  emit ticked();
}

void Board::began(const QJsonObject &what) {
  QStringList words{what.value("cmd").toString()};
  for (const QJsonValue &a : what.value("args").toArray()) words.append(a.toString());
  working_ = true;
  what_ = words.join(' ') + " (" + what.value("actor").toString() + ")";
  emit changed();
}

void Board::ended(const QJsonObject &ev) {
  const QString summary = ev.value("summary").toString();
  const QString said = ev.value("actor").toString() == "user" ? summary : "agent: " + summary;
  working_ = false;
  what_.clear();
  // the user's own failed action already shows its whole error, whose first line this is
  if (said != line_.section('\n', 0, 0)) line_ = said;
  owesLine_ = false;
  emit changed();
  ++turnsOwed_;
  refresh();
}

void Board::feedWent(bool up) {
  connected_ = up;
  if (up) {
    lost_ = false;
    emit changed();
    if (needStatus_) readStatus();
    return;
  }
  // a request that was running ends unseen if the service went with it; one still running holds
  // the service, so the feed can't come back before it is over
  working_ = false;
  what_.clear();
  line_ = lostLine;
  lost_ = needStatus_ = owesLine_ = true;
  emit changed();
}

void Board::readStatus() {
  read("status", {}, {}, [this](const Reply &r) { tookStatus(r); });
}

void Board::tookStatus(const Reply &r) {
  if (r.ok) {
    showStatus(r);
  } else if (!lost_ && r.error != line_) {  // while the feed is lost, the line already says so
    line_ = r.error;
    emit changed();
  }
  if (std::exchange(statusAgain_, false)) refresh();
  else turn();
}

void Board::showStatus(const Reply &r) {
  const QJsonObject o = r.result.toObject();
  const auto attached = attachedOf(o.value("attached"));
  const auto hunt = huntOf(o.value("hunt"));
  const QString counts = hunt ? r.text.section('\n', 1) : QString();
  QString line = line_;
  if (o.contains("closed")) line = r.text;  // the game went: say so, whatever was said before
  else if (owesLine_) line = r.text.section('\n', 0, 0);
  needStatus_ = owesLine_ = false;
  if (attached == attached_ && hunt == hunt_ && counts == counts_ && line == line_) return;
  attached_ = attached;
  hunt_ = hunt;
  counts_ = counts;
  line_ = line;
  emit changed();
}

// after an event: a status read sent after it, then `turned`. A read already out was sent before
// the event, so one more follows when it returns (one, however many events came meanwhile).
void Board::refresh() {
  if (reading_.contains(statusKey())) {
    statusAgain_ = true;
    return;
  }
  if (working_ || starting_) {  // no read can go now: lists are told at once and read when they can
    turn();
    return;
  }
  readStatus();
}

void Board::turn() {
  for (int n = std::exchange(turnsOwed_, 0); n > 0; --n) emit turned();
}
