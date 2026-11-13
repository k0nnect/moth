#include "fake_core.hpp"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTest>

namespace {

// the commands that change nothing, so may be sent with `record: false` (as the daemon has them)
const QStringList reads = {"bottles", "ps", "status", "results", "kept", "log", "games"};

QJsonObject optsObject(const Opts &opts) {
  QJsonObject o;
  for (const auto &[k, v] : opts) o.insert(k, v);
  return o;
}

QString firstLine(const QString &s) { return s.section('\n', 0, 0); }

}  // namespace

FakeCore::FakeCore(const QString &home) : path_(home + "/core.sock") {
  QObject::connect(&server_, &QLocalServer::newConnection, this, [this] { accept(); });
  start();
}

FakeCore::~FakeCore() { stop(); }

void FakeCore::on(const QString &cmd, std::function<Reply(const QStringList &, const Opts &)> answer) {
  answers_.insert(cmd, std::move(answer));
}

void FakeCore::hold(const QString &cmd) { holding_.insert(cmd); }

void FakeCore::release(const QString &cmd) {
  holding_.remove(cmd);
  QList<Held> now;
  for (qsizetype i = 0; i < held_.size();) {
    if (held_[i].request.cmd == cmd) now.append(held_.takeAt(i));
    else ++i;
  }
  for (const Held &h : now)
    if (h.socket) answer(h.socket, h.id, h.request);
}

void FakeCore::push(const QJsonObject &feedLine) {
  if (feedLine.value("event").isObject())
    seq_ = std::max(seq_, feedLine.value("event").toObject().value("seq").toInteger());
  for (const auto &w : watchers_)
    if (w) send(w, feedLine);
}

void FakeCore::dropWatchers() {
  const auto ws = std::exchange(watchers_, {});
  for (const auto &w : ws)
    if (w) w->abort();
}

void FakeCore::stop() {
  server_.close();
  QLocalServer::removeServer(path_);
  watchers_.clear();
  held_.clear();
  for (const auto &c : std::exchange(connections_, {}))
    if (c) {
      c->abort();
      c->deleteLater();
    }
}

void FakeCore::start() {
  QLocalServer::removeServer(path_);
  if (!server_.listen(path_)) qFatal("fake core can't listen at %s: %s", qPrintable(path_), qPrintable(server_.errorString()));
}

QList<FakeCore::Seen> FakeCore::seen() const { return seen_; }

int FakeCore::watchers() const {
  int n = 0;
  for (const auto &w : watchers_)
    if (w && w->state() == QLocalSocket::ConnectedState) ++n;
  return n;
}

QList<Opts> FakeCore::watches() const { return watches_; }

void FakeCore::feedRequests(bool on) { feedRequests_ = on; }

void FakeCore::refuseWatch(const QString &error) { refuseWatch_ = error; }

void FakeCore::closeGame(const QString &name) { closing_ = name; }

void FakeCore::accept() {
  while (QLocalSocket *s = server_.nextPendingConnection()) {
    connections_.append(s);
    auto taken = std::make_shared<bool>(false);
    QObject::connect(s, &QLocalSocket::readyRead, this, [this, s, taken] {
      if (*taken) {
        s->readAll();  // nothing more is expected on a connection
        return;
      }
      if (!s->canReadLine()) return;
      *taken = true;
      heard(s, s->readLine().trimmed());
    });
    QObject::connect(s, &QLocalSocket::disconnected, this, [this, s] {
      watchers_.removeAll(s);
      connections_.removeAll(s);
      s->deleteLater();
    });
  }
}

void FakeCore::heard(QLocalSocket *socket, const QByteArray &line) {
  const QJsonObject r = QJsonDocument::fromJson(line).object();
  const QJsonValue id = r.value("id");
  Seen req;
  req.actor = r.value("actor").toString("user");
  req.cmd = r.value("cmd").toString();
  for (const QJsonValue &a : r.value("args").toArray()) req.args.append(a.toString());
  const QJsonObject opts = r.value("opts").toObject();
  for (auto it = opts.begin(); it != opts.end(); ++it) req.opts.append({it.key(), it.value().toString()});
  req.record = r.value("record") != QJsonValue(false);

  if (req.cmd == "watch") {
    watches_.append(req.opts);
    if (!refuseWatch_.isEmpty()) {
      send(socket, {{"id", id}, {"ok", false}, {"error", refuseWatch_}});
      socket->disconnectFromServer();
      return;
    }
    watchers_.append(socket);
    send(socket, {{"id", id}, {"ok", true}, {"result", QJsonObject{{"seq", seq_}}}, {"text", ""}});
    return;
  }
  seen_.append(req);
  if (!req.record && !reads.contains(req.cmd)) {
    send(socket, {{"id", id}, {"ok", false}, {"error", "only commands that change nothing can go unrecorded"}});
    socket->disconnectFromServer();
    return;
  }
  if (req.record && feedRequests_)
    push({{"busy", QJsonObject{{"actor", req.actor}, {"cmd", req.cmd}, {"args", QJsonArray::fromStringList(req.args)}}}});
  if (holding_.contains(req.cmd)) held_.append({socket, id, req});
  else answer(socket, id, req);
}

void FakeCore::answer(QLocalSocket *socket, const QJsonValue &id, const Seen &req) {
  Reply r;
  // the daemon's closed-game status: recorded though it was asked not to be (R4)
  const bool closes = req.cmd == "status" && !req.record && !closing_.isEmpty();
  if (closes) {
    r = {true, QJsonObject{{"attached", QJsonValue::Null}, {"hunt", QJsonValue::Null}, {"closed", closing_}},
         "not attached. " + closing_ + " closed.", {}};
    closing_.clear();
    answers_.remove("status");
  } else if (answers_.contains(req.cmd)) {
    r = answers_.value(req.cmd)(req.args, req.opts);
  } else if (req.cmd == "status") {
    r = {true, QJsonObject{{"attached", QJsonValue::Null}, {"hunt", QJsonValue::Null}}, "not attached", {}};
  } else {
    r.error = "unknown command: " + req.cmd + ". run: moth help";
  }

  if ((req.record && feedRequests_) || closes) {
    QJsonObject ev{{"seq", ++seq_},
                   {"ts", QDateTime::currentMSecsSinceEpoch() / 1000.0},
                   {"actor", req.actor},
                   {"cmd", req.cmd},
                   {"args", QJsonArray::fromStringList(req.args)},
                   {"ok", r.ok},
                   {"summary", firstLine(r.ok ? r.text : r.error)}};
    if (!req.opts.isEmpty()) ev.insert("opts", optsObject(req.opts));
    push({{"event", ev}});
  }
  if (r.ok) send(socket, {{"id", id}, {"ok", true}, {"result", r.result.isUndefined() ? QJsonValue() : r.result}, {"text", r.text}});
  else send(socket, {{"id", id}, {"ok", false}, {"error", r.error}});
  socket->disconnectFromServer();
}

void FakeCore::send(QLocalSocket *socket, const QJsonObject &line) {
  socket->write(QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
  socket->flush();
}

FakeMoth::FakeMoth(const QString &dir) : path(dir + "/fake-moth"), marker(dir + "/marker") {
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) qFatal("can't write %s", qPrintable(path));
  f.write("#!/bin/sh\necho \"$1 $MOTH_HOME\" >> \"" + marker.toUtf8() + "\"\n");
  f.close();
  f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

QStringList FakeMoth::runs() const {
  QFile f(marker);
  if (!f.open(QIODevice::ReadOnly)) return {};
  return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
}

bool Rig::ready() {
  return QTest::qWaitFor([this] { return board.connected() && !board.line().isEmpty(); });
}

int timesSeen(const FakeCore &fake, const QString &cmd) {
  int n = 0;
  for (const auto &s : fake.seen())
    if (s.cmd == cmd) ++n;
  return n;
}
