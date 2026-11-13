#include "board.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>

namespace {

const QString lostLine = QString::fromUtf8("lost moth's background service. trying again…");
const QString stoppedLine = "moth's background service was stopped. it starts again with your next action.";

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
  connect(core, &CoreClient::feedRefused, this, &Board::refused);
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

QString Board::home() const { return core_->home(); }

QString Board::busyText() const {
  if (!running_.isEmpty()) return "moth is busy: " + running_;
  if (working_) return "moth is busy: " + what_;
  if (starting_) return QString::fromUtf8("moth is starting…");
  return {};
}

bool Board::run(const QString &cmd, const QStringList &args, const Opts &opts, std::function<void(const Reply &)> then) {
  // one of the user's own at a time, even before the feed says the first has started
  if (!running_.isEmpty() || working_) {
    say(busyText());
    return false;
  }
  running_ = (QStringList{cmd} + args).join(' ');
  core_->call(cmd, args, opts, true, [self = QPointer<Board>(this), then](const Reply &r) {
    if (!self) return;
    self->running_.clear();
    self->say(r.ok ? r.text : r.error);  // all its lines; its event (the first line) leaves them
    if (then) then(r);
  });
  return true;
}

bool Board::read(const QString &cmd, const QStringList &args, const Opts &opts, std::function<void(const Reply &)> done) {
  if (working_ || starting_) return false;
  ask(cmd, args, opts, std::move(done));
  return true;
}

// an unrecorded read, unless the same one is still out
void Board::ask(const QString &cmd, const QStringList &args, const Opts &opts, std::function<void(const Reply &)> done) {
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
  // the stop's own event comes just before the feed drops: the drop is then said as a stop
  stopped_ = ev.value("cmd").toString() == "stop" && ev.value("ok").toBool();
  // the user's own action already shows its whole answer, whose first line this is
  if (said != line_.section('\n', 0, 0)) line_ = said;
  owesLine_ = false;
  emit changed();
  ++turnsOwed_;
  refresh();
}

void Board::feedWent(bool up) {
  connected_ = up;
  if (up) {
    lost_ = stopped_ = false;
    // the line still shows why the feed was refused, which is over: the next status read is said
    if (!refusal_.isEmpty() && line_ == refusal_) needStatus_ = owesLine_ = true;
    refusal_.clear();
    emit changed();
    if (needStatus_) readStatus();
    return;
  }
  // a request that was running ends unseen if the service went with it; one still running holds
  // the service, so the feed can't come back before it is over
  working_ = false;
  what_.clear();
  line_ = stopped_ ? stoppedLine : lostLine;
  lost_ = needStatus_ = owesLine_ = true;
  emit changed();
}

// the core would not take the feed (such as `too many watchers`): said in its words; the client
// keeps trying
void Board::refused(const QString &error) {
  refusal_ = error;
  say(error);
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
// the event, so one more follows when it returns (one, however many events came meanwhile). This
// read goes even while a request runs (the core answers it when that is over) or while the start
// is under way, so `turned` always comes after fresh status.
void Board::refresh() {
  if (reading_.contains(statusKey())) {
    statusAgain_ = true;
    return;
  }
  ask("status", {}, {}, [this](const Reply &r) { tookStatus(r); });
}

void Board::turn() {
  for (int n = std::exchange(turnsOwed_, 0); n > 0; --n) emit turned();
}
