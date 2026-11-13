#include "core_client.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

namespace {

const char *const badReply = "moth's background service sent something that isn't a reply";

QByteArray lineOf(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n'; }

Reply failure(const QString &error) {
  Reply r;
  r.error = error;
  return r;
}

Reply replyOf(const QByteArray &line) {
  const QJsonDocument doc = QJsonDocument::fromJson(line);
  if (!doc.isObject()) return failure(badReply);
  const QJsonObject o = doc.object();
  if (o.value("ok") != QJsonValue(true))
    return failure(o.value("error").isString() ? o.value("error").toString() : QString(badReply));
  Reply r;
  r.ok = true;
  r.result = o.value("result");
  r.text = o.value("text").toString();
  return r;
}

}  // namespace

struct CoreClient::Exchange {
  QByteArray request;
  std::function<void(const Reply &)> done;
  bool retried = false;  // the service has been started once for it
};

CoreClient::CoreClient(QString home, QString mothBin, QObject *parent)
    : QObject(parent), home_(std::move(home)), mothBin_(std::move(mothBin)) {}

QString CoreClient::socketPath() const { return home_ + "/core.sock"; }

void CoreClient::call(const QString &cmd, const QStringList &args, const Opts &opts, bool record,
                      std::function<void(const Reply &)> done) {
  QJsonObject o;
  for (const auto &[k, v] : opts) o.insert(k, v);
  QJsonObject request{{"id", 1}, {"actor", "user"}, {"cmd", cmd}, {"args", QJsonArray::fromStringList(args)}, {"opts", o}};
  if (!record) request.insert("record", false);
  auto x = std::make_shared<Exchange>(Exchange{lineOf(request), std::move(done)});
  QTimer::singleShot(0, this, [this, x] { attempt(x); });
}

void CoreClient::attempt(const std::shared_ptr<Exchange> &x) {
  auto *s = new QLocalSocket(this);
  auto reached = std::make_shared<bool>(false);
  auto finish = [this, s, x](const Reply &r) {
    QObject::disconnect(s, nullptr, this, nullptr);
    s->abort();
    s->deleteLater();
    if (auto done = std::exchange(x->done, nullptr)) done(r);
  };
  connect(s, &QLocalSocket::connected, this, [s, x, reached] {
    *reached = true;
    s->write(x->request);
  });
  connect(s, &QLocalSocket::readyRead, this, [s, finish] {
    if (s->canReadLine()) finish(replyOf(s->readLine()));
  });
  auto ended = [this, s, x, reached, finish] {
    if (s->canReadLine()) {
      finish(replyOf(s->readLine()));
    } else if (*reached) {
      finish(failure("lost contact with moth's background service. see " + home_ + "/core.log"));
    } else if (x->retried) {
      finish(failure("moth's background service is not answering. see " + home_ + "/core.log"));
    } else {
      x->retried = true;
      QObject::disconnect(s, nullptr, this, nullptr);
      s->deleteLater();
      startService([this, x] { attempt(x); });
    }
  };
  connect(s, &QLocalSocket::errorOccurred, this, ended);
  connect(s, &QLocalSocket::disconnected, this, ended);
  s->connectToServer(socketPath());
}

// `moth status` starts the service when it isn't running. One runs at a time; every request that
// found no service waits for it to end (10 seconds at most), then tries once more.
void CoreClient::startService(std::function<void()> then) {
  waiting_.append(std::move(then));
  if (starting_) return;
  auto *p = new QProcess(this);
  starting_ = p;
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  env.insert("MOTH_HOME", home_);
  p->setProcessEnvironment(env);
  p->setStandardInputFile(QProcess::nullDevice());
  p->setStandardOutputFile(QProcess::nullDevice());
  p->setStandardErrorFile(QProcess::nullDevice());
  auto over = [this, p] {
    if (starting_ != p) return;
    starting_ = nullptr;
    QObject::disconnect(p, nullptr, this, nullptr);
    if (p->state() != QProcess::NotRunning) {
      p->kill();
      p->waitForFinished(1000);
    }
    p->deleteLater();
    for (const auto &retry : std::exchange(waiting_, {})) retry();
  };
  connect(p, &QProcess::finished, this, over);
  connect(
      p, &QProcess::errorOccurred, this, [over](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) over();
      },
      Qt::QueuedConnection);
  QTimer::singleShot(10000, p, over);
  p->start(mothBin_, {"status"});
}

void CoreClient::startFeed() {
  if (feedStarted_) return;
  feedStarted_ = true;
  watch();
}

void CoreClient::watch() {
  auto *s = new QLocalSocket(this);
  auto answered = std::make_shared<bool>(false);
  connect(s, &QLocalSocket::connected, this, [this, s] {
    QJsonObject opts;
    if (lastSeq_ >= 0) opts.insert("after", QString::number(lastSeq_));
    s->write(lineOf({{"id", 1}, {"cmd", "watch"}, {"args", QJsonArray()}, {"opts", opts}}));
  });
  connect(s, &QLocalSocket::readyRead, this, [this, s, answered] { heard(s, *answered); });
  auto lost = [this, s] { dropFeed(s); };
  connect(s, &QLocalSocket::errorOccurred, this, lost);
  connect(s, &QLocalSocket::disconnected, this, lost);
  s->connectToServer(socketPath());
}

// watch's reply, then feed lines. A line not yet ended waits for the rest; one cut short when the
// service lets go of the feed is dropped with the connection.
void CoreClient::heard(QLocalSocket *feed, bool &answered) {
  while (feed->canReadLine()) {
    const QJsonObject line = QJsonDocument::fromJson(feed->readLine()).object();
    if (!answered) {
      answered = true;
      if (line.value("ok") != QJsonValue(true)) {  // such as `too many watchers`: try again later
        dropFeed(feed);
        return;
      }
      if (lastSeq_ < 0) lastSeq_ = line.value("result").toObject().value("seq").toInteger();
      setFeedUp(true);
    } else if (line.value("busy").isObject()) {
      emit busy(line.value("busy").toObject());
    } else if (line.value("event").isObject()) {
      const QJsonObject ev = line.value("event").toObject();
      lastSeq_ = ev.value("seq").toInteger(lastSeq_);
      emit event(ev);
    }
  }
}

void CoreClient::dropFeed(QLocalSocket *feed) {
  QObject::disconnect(feed, nullptr, this, nullptr);
  feed->abort();
  feed->deleteLater();
  setFeedUp(false);
  QTimer::singleShot(1000, this, [this] { watch(); });
}

void CoreClient::setFeedUp(bool up) {
  if (up == feedUp_) return;
  feedUp_ = up;
  emit feedUp(up);
}
