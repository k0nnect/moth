#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "core_client.hpp"
#include "fake_core.hpp"
#include "paths.hpp"

class TestCoreClient : public QObject {
  Q_OBJECT

private slots:
  void homeAndProgram();
  void sendsTheRequestAsGiven();
  void anErrorReplyIsTheCoresText();
  void feedSignalsInOrder();
  void feedComesBackAfterTheLastSeq();
  void startsTheServiceOnceThenSaysSo();
};

namespace {

// sets an environment variable for one scope, and puts back what was there
struct Env {
  QByteArray name, before;
  bool had;
  Env(const char *n, const QByteArray &value) : name(n), before(qgetenv(n)), had(qEnvironmentVariableIsSet(n)) {
    if (value.isNull()) qunsetenv(n);
    else qputenv(n, value);
  }
  ~Env() {
    if (had) qputenv(name.constData(), before);
    else qunsetenv(name.constData());
  }
};

}  // namespace

void TestCoreClient::homeAndProgram() {
  {
    Env home("MOTH_HOME", "/x/y");
    QCOMPARE(mothHome(), QString("/x/y"));
  }
  {
    Env home("MOTH_HOME", "rel");
    QCOMPARE(mothHome(), QDir::currentPath() + "/rel");
  }
  {
    Env home("MOTH_HOME", QByteArray());
    Env user("HOME", "/Users/someone");
    QCOMPARE(mothHome(), QString("/Users/someone/Library/Application Support/moth"));
  }
  {
    Env bin("MOTH_BIN", "/opt/moth/moth");
    QCOMPARE(mothBin(), QString("/opt/moth/moth"));
  }
  {
    Env bin("MOTH_BIN", QByteArray());
    QCOMPARE(mothBin(), QCoreApplication::applicationDirPath() + "/moth");
  }
}

void TestCoreClient::sendsTheRequestAsGiven() {
  QTemporaryDir home("/tmp/moth-app-XXXXXX");
  FakeCore fake(home.path());
  fake.on("results", [](const QStringList &, const Opts &) {
    return Reply{true, QJsonObject{{"total", 3}}, "3 matches", {}};
  });
  CoreClient core(home.path(), "/usr/bin/true");

  int answers = 0;
  Reply got;
  core.call("results", {}, {{"all", "true"}, {"n", "200"}}, false, [&](const Reply &r) {
    ++answers;
    got = r;
  });
  QTRY_COMPARE(answers, 1);
  QVERIFY(got.ok);
  QCOMPARE(got.result.toObject().value("total").toInt(), 3);
  QCOMPARE(got.text, QString("3 matches"));
  QCOMPARE(fake.seen().size(), 1);
  const auto unrecorded = fake.seen().at(0);
  QCOMPARE(unrecorded.cmd, QString("results"));
  QCOMPARE(unrecorded.actor, QString("user"));
  QCOMPARE(unrecorded.args, QStringList());
  QCOMPARE(unrecorded.opts, (Opts{{"all", "true"}, {"n", "200"}}));
  QCOMPARE(unrecorded.record, false);

  core.call("find", {"100"}, {}, true, [&](const Reply &) { ++answers; });
  QTRY_COMPARE(answers, 2);
  const auto recorded = fake.seen().at(1);
  QCOMPARE(recorded.cmd, QString("find"));
  QCOMPARE(recorded.args, QStringList{"100"});
  QCOMPARE(recorded.actor, QString("user"));
  QCOMPARE(recorded.record, true);

  QTest::qWait(100);
  QCOMPARE(answers, 2);  // each `done` once
}

void TestCoreClient::anErrorReplyIsTheCoresText() {
  QTemporaryDir home("/tmp/moth-app-XXXXXX");
  FakeCore fake(home.path());
  fake.on("find", [](const QStringList &, const Opts &) { return Reply{false, {}, {}, "132.5.1 is not a number"}; });
  CoreClient core(home.path(), "/usr/bin/true");

  std::optional<Reply> got;
  core.call("find", {"132.5.1"}, {}, true, [&](const Reply &r) { got = r; });
  QTRY_VERIFY(got.has_value());
  QVERIFY(!got->ok);
  QCOMPARE(got->error, QString("132.5.1 is not a number"));
}

void TestCoreClient::feedSignalsInOrder() {
  QTemporaryDir home("/tmp/moth-app-XXXXXX");
  FakeCore fake(home.path());
  CoreClient core(home.path(), "/usr/bin/true");
  QSignalSpy up(&core, &CoreClient::feedUp);
  QStringList order;
  QJsonObject busy, event;
  connect(&core, &CoreClient::busy, this, [&](const QJsonObject &w) { order << "busy"; busy = w; });
  connect(&core, &CoreClient::event, this, [&](const QJsonObject &e) { order << "event"; event = e; });

  core.startFeed();
  QTRY_COMPARE(fake.watchers(), 1);
  QTRY_COMPARE(up.count(), 1);
  QCOMPARE(up.at(0).at(0).toBool(), true);

  fake.push({{"busy", QJsonObject{{"actor", "agent"}, {"cmd", "find"}, {"args", QJsonArray{"100"}}}}});
  fake.push({{"event", QJsonObject{{"seq", 12}, {"actor", "agent"}, {"cmd", "find"}, {"args", QJsonArray{"100"}},
                                   {"ok", true}, {"summary", "241,178 matches"}}}});
  QTRY_COMPARE(order, (QStringList{"busy", "event"}));
  QCOMPARE(busy.value("cmd").toString(), QString("find"));
  QCOMPARE(busy.value("actor").toString(), QString("agent"));
  QCOMPARE(event.value("seq").toInt(), 12);
  QCOMPARE(event.value("summary").toString(), QString("241,178 matches"));
  QCOMPARE(fake.seen().size(), 0);  // watch is not a request the fake lists
}

void TestCoreClient::feedComesBackAfterTheLastSeq() {
  QTemporaryDir home("/tmp/moth-app-XXXXXX");
  FakeCore fake(home.path());
  CoreClient core(home.path(), "/usr/bin/true");
  QSignalSpy up(&core, &CoreClient::feedUp);
  QSignalSpy events(&core, &CoreClient::event);

  core.startFeed();
  QTRY_COMPARE(fake.watchers(), 1);
  QCOMPARE(fake.watches().size(), 1);
  fake.push({{"event", QJsonObject{{"seq", 12}, {"actor", "user"}, {"cmd", "find"}, {"ok", true}, {"summary", "x"}}}});
  QTRY_COMPARE(events.count(), 1);

  up.clear();
  fake.dropWatchers();
  QTRY_COMPARE(up.count(), 1);
  QCOMPARE(up.at(0).at(0).toBool(), false);
  QTRY_COMPARE_WITH_TIMEOUT(fake.watchers(), 1, 2000);
  QCOMPARE(fake.watches().size(), 2);
  QCOMPARE(fake.watches().at(1), (Opts{{"after", "12"}}));
  QTRY_COMPARE(up.count(), 2);
  QCOMPARE(up.at(1).at(0).toBool(), true);
}

void TestCoreClient::startsTheServiceOnceThenSaysSo() {
  QTemporaryDir home("/tmp/moth-app-XXXXXX");
  const QString marker = home.filePath("marker");
  const QString script = home.filePath("fake-moth");
  {
    QFile f(script);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("#!/bin/sh\necho \"$1 $MOTH_HOME\" >> \"" + marker.toUtf8() + "\"\n");
    f.close();
    QVERIFY(f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
  }
  Env bin("MOTH_BIN", script.toUtf8());
  CoreClient core(home.path(), mothBin());

  std::optional<Reply> got;
  core.call("status", {}, {}, false, [&](const Reply &r) { got = r; });
  QTRY_VERIFY(got.has_value());
  QVERIFY(!got->ok);
  QCOMPARE(got->error, "moth's background service is not answering. see " + home.path() + "/core.log");

  QFile m(marker);
  QVERIFY(m.open(QIODevice::ReadOnly));
  QCOMPARE(QString::fromUtf8(m.readAll()), "status " + home.path() + "\n");  // once, with this home
}

QTEST_GUILESS_MAIN(TestCoreClient)
#include "test_core_client.moc"
