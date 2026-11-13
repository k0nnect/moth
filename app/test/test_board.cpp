#include <QJsonArray>
#include <QSignalSpy>
#include <QtTest>

#include "board.hpp"
#include "fake_core.hpp"

class TestBoard : public QObject {
  Q_OBJECT

private slots:
  void busyRefusesARun();
  void anAgentsEventIsSaidAndReadsStatus();
  void theGameClosing();
  void theServiceStoppingAndComingBack();
  void aFailedRunSaysItsError();
  void ticksDoNotPileUp();
  void huntFromStatus();
};

namespace {

const QString lost = QString::fromUtf8("lost moth's background service. trying again…");

QJsonObject busyLine(const QString &actor, const QString &cmd, const QStringList &args) {
  return {{"busy", QJsonObject{{"actor", actor}, {"cmd", cmd}, {"args", QJsonArray::fromStringList(args)}}}};
}

QJsonObject eventLine(qint64 seq, const QString &actor, const QString &cmd, const QStringList &args, const QString &summary) {
  return {{"event", QJsonObject{{"seq", seq}, {"ts", 1794518854.4}, {"actor", actor}, {"cmd", cmd},
                                {"args", QJsonArray::fromStringList(args)}, {"ok", true}, {"summary", summary}}}};
}

const QJsonObject disco{{"pid", 4242}, {"name", "disco.exe"}, {"bits", 64}, {"bottle", "uhmm"}};
const QString discoLine = "attached to disco.exe (pid 4242, 64-bit) in bottle uhmm";

}  // namespace

// Review Focus 1: while someone's find runs, a click starts nothing and says what is running
void TestBoard::busyRefusesARun() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.push(busyLine("agent", "find", {"100"}));
  QTRY_VERIFY(rig.board.working());
  QCOMPARE(rig.board.what(), QString("find 100 (agent)"));

  const int before = rig.fake.seen().size();
  int answered = 0;
  rig.board.run("next", {"down"}, {}, [&](const Reply &) { ++answered; });
  QCOMPARE(rig.board.line(), QString("moth is busy: find 100 (agent)"));
  QTest::qWait(200);
  QCOMPARE(rig.fake.seen().size(), before);
  QCOMPARE(timesSeen(rig.fake, "next"), 0);
  QCOMPARE(answered, 0);
}

void TestBoard::anAgentsEventIsSaidAndReadsStatus() {
  Rig rig;
  QVERIFY(rig.ready());
  QSignalSpy turned(&rig.board, &Board::turned);
  rig.fake.push(busyLine("agent", "find", {"100"}));
  QTRY_VERIFY(rig.board.working());
  const int statuses = timesSeen(rig.fake, "status");

  rig.fake.push(eventLine(12, "agent", "find", {"100"}, "241,178 matches"));
  QTRY_COMPARE(rig.board.line(), QString("agent: 241,178 matches"));
  QVERIFY(!rig.board.working());
  QCOMPARE(rig.board.what(), QString());
  QTRY_COMPARE(timesSeen(rig.fake, "status"), statuses + 1);
  QCOMPARE(rig.fake.seen().last().cmd, QString("status"));
  QCOMPARE(rig.fake.seen().last().record, false);
  QTest::qWait(200);
  QCOMPARE(timesSeen(rig.fake, "status"), statuses + 1);
  QCOMPARE(turned.count(), 1);
  QCOMPARE(rig.board.line(), QString("agent: 241,178 matches"));  // the status read leaves it
}

// Review Focus 3: the game closes mid-hunt
void TestBoard::theGameClosing() {
  Rig rig;
  rig.fake.on("status", [](const QStringList &, const Opts &) {
    const QJsonObject hunt{{"main", 3}, {"wide", 0}, {"groups", QJsonArray()}, {"blind", false},
                           {"by_tag", QJsonArray{QJsonArray{"whole number", 3}}}, {"can_compare", true}};
    return Reply{true, QJsonObject{{"attached", disco}, {"hunt", hunt}}, discoLine + "\n3 matches", {}};
  });
  QVERIFY(rig.ready());
  QVERIFY(rig.board.attached().has_value());
  QVERIFY(rig.board.hunt().has_value());
  QCOMPARE(rig.board.counts(), QString("3 matches"));

  rig.fake.on("status", [](const QStringList &, const Opts &) {
    return Reply{true, QJsonObject{{"attached", QJsonValue::Null}, {"hunt", QJsonValue::Null}, {"closed", "disco.exe"}},
                 "not attached. disco.exe closed.", {}};
  });
  QSignalSpy changed(&rig.board, &Board::changed);
  rig.board.tick();
  QTRY_VERIFY(!rig.board.attached().has_value());
  QVERIFY(!rig.board.hunt().has_value());
  QCOMPARE(rig.board.counts(), QString());
  QCOMPARE(rig.board.line(), QString("not attached. disco.exe closed."));
  QVERIFY(changed.count() >= 1);
}

// Review Focus 4: the service stops and comes back while the window is open
void TestBoard::theServiceStoppingAndComingBack() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.stop();
  QTRY_VERIFY(!rig.board.connected());
  QCOMPARE(rig.board.line(), lost);

  rig.fake.start();
  QTRY_VERIFY_WITH_TIMEOUT(rig.board.connected(), 2000);
  std::optional<Reply> got;
  rig.board.run("status", {}, {}, [&](const Reply &r) { got = r; });
  QTRY_VERIFY(got.has_value());
  QVERIFY(got->ok);
  const auto seen = rig.fake.seen();
  QVERIFY(std::any_of(seen.begin(), seen.end(), [](const FakeCore::Seen &s) { return s.cmd == "status" && s.record; }));
}

void TestBoard::aFailedRunSaysItsError() {
  // no service at all, and starting one does nothing: the client's own error is the line
  QTemporaryDir home("/tmp/moth-app-XXXXXX");
  CoreClient core(home.path(), "/usr/bin/true");
  Board board(&core);
  std::optional<Reply> got;
  board.run("find", {"100"}, {}, [&](const Reply &r) { got = r; });
  QTRY_VERIFY(got.has_value());
  QVERIFY(!got->ok);
  QCOMPARE(board.line(), "moth's background service is not answering. see " + home.path() + "/core.log");
}

void TestBoard::ticksDoNotPileUp() {
  Rig rig;
  QVERIFY(rig.ready());
  QSignalSpy ticked(&rig.board, &Board::ticked);
  const int statuses = timesSeen(rig.fake, "status");
  rig.fake.hold("status");
  rig.board.tick();
  rig.board.tick();
  QTRY_COMPARE(timesSeen(rig.fake, "status"), statuses + 1);
  QTest::qWait(200);
  QCOMPARE(timesSeen(rig.fake, "status"), statuses + 1);
  QCOMPARE(ticked.count(), 2);

  rig.fake.release("status");
  QTest::qWait(100);
  rig.board.tick();  // the read came back, so the next tick reads again
  QTRY_COMPARE(timesSeen(rig.fake, "status"), statuses + 2);
}

void TestBoard::huntFromStatus() {
  Rig rig;
  rig.fake.on("status", [](const QStringList &, const Opts &) {
    const QJsonObject hunt{{"main", 241178}, {"wide", 5000000000LL}, {"groups", QJsonArray()}, {"blind", true},
                           {"by_tag", QJsonArray{QJsonArray{"whole number", 241178}, QJsonArray{"decimal", 12}}},
                           {"can_compare", false}};
    return Reply{true, QJsonObject{{"attached", disco}, {"hunt", hunt}},
                 discoLine + "\n241,178 matches\n5,000,000,000 more in rarer forms", {}};
  });
  QVERIFY(rig.ready());
  const auto &a = rig.board.attached();
  QVERIFY(a.has_value());
  QCOMPARE(a->pid, 4242);
  QCOMPARE(a->name, QString("disco.exe"));
  QCOMPARE(a->bits, 64);
  QCOMPARE(a->bottle, QString("uhmm"));
  const auto &h = rig.board.hunt();
  QVERIFY(h.has_value());
  QCOMPARE(h->main, 241178);
  QCOMPARE(h->wide, 5000000000LL);
  QCOMPARE(h->byTag, (QList<QPair<QString, qint64>>{{"whole number", 241178}, {"decimal", 12}}));
  QCOMPARE(h->blind, true);
  QCOMPARE(h->canCompare, false);
  QCOMPARE(rig.board.counts(), QString("241,178 matches\n5,000,000,000 more in rarer forms"));
  QCOMPARE(rig.board.line(), discoLine);  // the first status read says where things stand
}

QTEST_GUILESS_MAIN(TestBoard)
#include "test_board.moc"
