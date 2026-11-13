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
  void theLaunchAndTheUsersOwnRequestsStartTheService();
  void pollingNeverStartsTheService();
  void aFailedActionKeepsItsWholeError();
  void anActionShowsItsWholeAnswer();
  void anEventAlwaysBringsFreshStatus();
  void anEventsStatusReadIsNeverSkipped();
  void aRefusedWatchIsSaid();
  void readsWaitWhileARequestRuns();
  void ticksDoNotPileUp();
  void readSaysWhetherAReplyWillCome();
  void huntFromStatus();
  void twoRunsBeforeTheFeedSaysSendOne();
  void busyTextSaysWhatStandsInTheWay();
  void theGameClosingAsTheCoreRecordsIt();
  void aStopFromElsewhereIsSaidAsAStop();
};

namespace {

const QString lost = QString::fromUtf8("lost moth's background service. trying again…");

QJsonObject busyLine(const QString &actor, const QString &cmd, const QStringList &args) {
  return {{"busy", QJsonObject{{"actor", actor}, {"cmd", cmd}, {"args", QJsonArray::fromStringList(args)}}}};
}

QJsonObject eventLine(qint64 seq, const QString &actor, const QString &cmd, const QStringList &args, const QString &summary,
                      bool ok = true) {
  return {{"event", QJsonObject{{"seq", seq}, {"ts", 1794518854.4}, {"actor", actor}, {"cmd", cmd},
                                {"args", QJsonArray::fromStringList(args)}, {"ok", ok}, {"summary", summary}}}};
}

QString notAnswering(const QTemporaryDir &home) {
  return "moth's background service is not answering. see " + home.path() + "/core.log";
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

// R7: with no service, the app starts it once at launch, and after that only the user's own
// requests start it, once each
void TestBoard::theLaunchAndTheUsersOwnRequestsStartTheService() {
  QTemporaryDir home("/tmp/moth-app-XXXXXX");
  FakeMoth moth(home.path());  // starts nothing, so no service ever answers
  CoreClient core(home.path(), moth.path);
  Board board(&core);
  QTRY_COMPARE(board.line(), notAnswering(home));  // the start at launch failed, and says so
  QCOMPARE(moth.runs(), QStringList{"status " + home.path()});

  board.tick();
  QTest::qWait(200);
  QCOMPARE(moth.runs().size(), 1);

  std::optional<Reply> got;
  board.run("find", {"100"}, {}, [&](const Reply &r) { got = r; });
  QTRY_VERIFY(got.has_value());
  QVERIFY(!got->ok);
  QCOMPARE(got->error, notAnswering(home));
  QCOMPARE(moth.runs().size(), 2);
  QCOMPARE(board.line(), notAnswering(home));
}

// R7: after the service went away, polls never start it again; the user's next action does, once
void TestBoard::pollingNeverStartsTheService() {
  QTemporaryDir home("/tmp/moth-app-XXXXXX");
  FakeMoth moth(home.path());
  FakeCore fake(home.path());
  CoreClient core(home.path(), moth.path);
  Board board(&core);
  QVERIFY(QTest::qWaitFor([&] { return board.connected() && !board.line().isEmpty(); }));
  QCOMPARE(moth.runs(), QStringList());  // the service answered at launch: nothing to start

  fake.stop();
  QTRY_VERIFY(!board.connected());
  for (int i = 0; i < 3; ++i) {
    board.tick();
    QTest::qWait(150);
  }
  QCOMPARE(moth.runs(), QStringList());
  QCOMPARE(board.line(), lost);  // a poll that finds no service leaves that said

  std::optional<Reply> got;
  board.run("find", {"100"}, {}, [&](const Reply &r) { got = r; });
  QTRY_VERIFY(got.has_value());
  QCOMPARE(moth.runs(), QStringList{"status " + home.path()});
  QCOMPARE(board.line(), notAnswering(home));
}

// R8: the user's own failed action shows its whole error, whether its event (whose summary is the
// error's first line) comes before the reply or after it
void TestBoard::aFailedActionKeepsItsWholeError() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.feedRequests(false);  // this test sends the events itself, in the order it wants
  const QString first = "row 3 is not at 0x7ffe1234 any more: the list changed.";
  const QString whole = first + "\nlook again: moth results";
  rig.fake.on("set", [&](const QStringList &, const Opts &) { return Reply{false, {}, {}, whole}; });
  QSignalSpy turned(&rig.board, &Board::turned);

  // the event, then the reply
  rig.fake.hold("set");
  std::optional<Reply> got;
  rig.board.run("set", {"3", "9"}, {{"at", "0x7ffe1234"}}, [&](const Reply &r) { got = r; });
  QTRY_COMPARE(timesSeen(rig.fake, "set"), 1);
  rig.fake.push(eventLine(1, "user", "set", {"3", "9"}, first, false));
  QTRY_COMPARE(rig.board.line(), first);
  rig.fake.release("set");
  QTRY_VERIFY(got.has_value());
  QCOMPARE(rig.board.line(), whole);

  // the reply, then the event
  rig.board.say("something else");
  got.reset();
  rig.board.run("set", {"3", "9"}, {{"at", "0x7ffe1234"}}, [&](const Reply &r) { got = r; });
  QTRY_VERIFY(got.has_value());
  QCOMPARE(rig.board.line(), whole);
  const int turns = turned.count();
  rig.fake.push(eventLine(2, "user", "set", {"3", "9"}, first, false));
  QTRY_COMPARE(turned.count(), turns + 1);  // the event has been taken
  QCOMPARE(rig.board.line(), whole);
}

// R13: the user's own action that worked shows the core's whole answer, all its lines, whether its
// event (whose summary is the first line) comes before the reply or after it; someone else's
// event still shows its first line, as theirs
void TestBoard::anActionShowsItsWholeAnswer() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.feedRequests(false);  // this test sends the events itself, in the order it wants
  const QString first = "set #3 to 9";
  const QString whole = first + "\nthe game put it straight back to 100. it's worked out from something else, so writing here won't stick.";
  rig.fake.on("set", [&](const QStringList &, const Opts &) {
    return Reply{true, QJsonObject{{"message", whole}, {"held", false}}, whole, {}};
  });
  QSignalSpy turned(&rig.board, &Board::turned);

  // the event, then the reply
  rig.fake.hold("set");
  std::optional<Reply> got;
  rig.board.run("set", {"3", "9"}, {{"at", "0x7ffe1234"}}, [&](const Reply &r) { got = r; });
  QTRY_COMPARE(timesSeen(rig.fake, "set"), 1);
  rig.fake.push(eventLine(1, "user", "set", {"3", "9"}, first));
  QTRY_COMPARE(rig.board.line(), first);
  rig.fake.release("set");
  QTRY_VERIFY(got.has_value());
  QCOMPARE(rig.board.line(), whole);

  // the reply, then the event
  rig.board.say("something else");
  got.reset();
  rig.board.run("set", {"3", "9"}, {{"at", "0x7ffe1234"}}, [&](const Reply &r) { got = r; });
  QTRY_VERIFY(got.has_value());
  QCOMPARE(rig.board.line(), whole);
  const int turns = turned.count();
  rig.fake.push(eventLine(2, "user", "set", {"3", "9"}, first));
  QTRY_COMPARE(turned.count(), turns + 1);  // the event has been taken
  QCOMPARE(rig.board.line(), whole);

  rig.fake.push(eventLine(3, "agent", "set", {"3", "9"}, first));
  QTRY_COMPARE(rig.board.line(), "agent: " + first);
}

// R9: an event always brings a status read sent after it, and `turned` once that is in
void TestBoard::anEventAlwaysBringsFreshStatus() {
  Rig rig;
  QVERIFY(rig.ready());
  int answered = 0;
  rig.fake.on("status", [&](const QStringList &, const Opts &) {
    // the read sent before the event sees the game as it was; the one after, as the event left it
    if (++answered == 1)
      return Reply{true, QJsonObject{{"attached", QJsonValue::Null}, {"hunt", QJsonValue::Null}}, "not attached", {}};
    return Reply{true, QJsonObject{{"attached", disco}, {"hunt", QJsonValue::Null}}, discoLine, {}};
  });
  QList<bool> attachedAtTurn;
  connect(&rig.board, &Board::turned, this, [&] { attachedAtTurn << rig.board.attached().has_value(); });

  const int statuses = timesSeen(rig.fake, "status");
  rig.fake.hold("status");
  rig.board.tick();
  QTRY_COMPARE(timesSeen(rig.fake, "status"), statuses + 1);
  rig.fake.push(eventLine(5, "agent", "attach", {"disco.exe"}, discoLine));
  QTRY_COMPARE(rig.board.line(), "agent: " + discoLine);
  QTest::qWait(100);
  QCOMPARE(timesSeen(rig.fake, "status"), statuses + 1);  // nothing more while that one is out
  QCOMPARE(attachedAtTurn.size(), 0);                     // and `turned` waits

  rig.fake.release("status");
  QTRY_COMPARE(timesSeen(rig.fake, "status"), statuses + 2);
  QTRY_COMPARE(attachedAtTurn.size(), 1);
  QCOMPARE(attachedAtTurn.at(0), true);  // the state the event caused is in when `turned` comes
  QTest::qWait(200);
  QCOMPARE(attachedAtTurn.size(), 1);
  QCOMPARE(timesSeen(rig.fake, "status"), statuses + 2);
}

// R9, back to back: someone attaches, then finds at once, while a tick's status read is out. The
// read after the attach still goes (the core answers it when it is free), and `turned` waits for it
void TestBoard::anEventsStatusReadIsNeverSkipped() {
  Rig rig;
  QVERIFY(rig.ready());
  int answered = 0;
  rig.fake.on("status", [&](const QStringList &, const Opts &) {
    if (++answered == 1)
      return Reply{true, QJsonObject{{"attached", QJsonValue::Null}, {"hunt", QJsonValue::Null}}, "not attached", {}};
    return Reply{true, QJsonObject{{"attached", disco}, {"hunt", QJsonValue::Null}}, discoLine, {}};
  });
  QList<bool> attachedAtTurn;
  connect(&rig.board, &Board::turned, this, [&] { attachedAtTurn << rig.board.attached().has_value(); });

  const int statuses = timesSeen(rig.fake, "status");
  rig.fake.hold("status");
  rig.board.tick();
  QTRY_COMPARE(timesSeen(rig.fake, "status"), statuses + 1);
  rig.fake.push(eventLine(5, "agent", "attach", {"disco.exe"}, discoLine));
  rig.fake.push(busyLine("agent", "find", {"100"}));
  QTRY_VERIFY(rig.board.working());
  QCOMPARE(attachedAtTurn.size(), 0);

  rig.fake.release("status");
  QTRY_COMPARE(timesSeen(rig.fake, "status"), statuses + 2);  // sent though the find is running
  QCOMPARE(rig.fake.seen().last().record, false);
  QTRY_COMPARE(attachedAtTurn.size(), 1);
  QCOMPARE(attachedAtTurn.at(0), true);
  QVERIFY(rig.board.working());
  QTest::qWait(200);
  QCOMPARE(attachedAtTurn.size(), 1);
}

// the core refusing the feed (`too many watchers`) is said in its words, at the start and after a
// loss, and the feed keeps trying
void TestBoard::aRefusedWatchIsSaid() {
  Rig rig;
  rig.fake.refuseWatch("too many watchers");
  QTRY_COMPARE(rig.board.line(), QString("too many watchers"));
  QTest::qWait(1500);  // the status read and the tries again leave it said
  QCOMPARE(rig.board.line(), QString("too many watchers"));
  QVERIFY(!rig.board.connected());

  rig.fake.refuseWatch({});
  QTRY_VERIFY_WITH_TIMEOUT(rig.board.connected(), 2000);
  QTRY_COMPARE(rig.board.line(), QString("not attached"));

  rig.fake.refuseWatch("too many watchers");
  rig.fake.dropWatchers();
  QTRY_COMPARE(rig.board.line(), lost);
  QTRY_COMPARE_WITH_TIMEOUT(rig.board.line(), QString("too many watchers"), 2000);
  QTest::qWait(1500);
  QCOMPARE(rig.board.line(), QString("too many watchers"));
}

// every panel relies on this: while a recorded request runs, a read sends nothing and never answers
void TestBoard::readsWaitWhileARequestRuns() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.push(busyLine("agent", "find", {"100"}));
  QTRY_VERIFY(rig.board.working());
  int answered = 0;
  QVERIFY(!rig.board.read("results", {}, {{"n", "200"}}, [&](const Reply &) { ++answered; }));  // dropped: no reply will come
  QTest::qWait(200);
  QCOMPARE(timesSeen(rig.fake, "results"), 0);
  QCOMPARE(answered, 0);
}

void TestBoard::readSaysWhetherAReplyWillCome() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.hold("results");
  int answered = 0;
  QVERIFY(rig.board.read("results", {}, {}, [&](const Reply &) { ++answered; }));  // sent
  QTRY_COMPARE(timesSeen(rig.fake, "results"), 1);
  QVERIFY(rig.board.read("results", {}, {}, [&](const Reply &) { ++answered; }));  // an identical one is out
  rig.fake.release("results");
  QTRY_COMPARE(answered, 1);
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

// F4: two presses of `undo last change` before the feed has said the first one started send one
// request; the second is refused in words, and its `then` is never called
void TestBoard::twoRunsBeforeTheFeedSaysSendOne() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.feedRequests(false);  // no busy line comes: the service is busy with something else
  rig.fake.on("undo", [](const QStringList &, const Opts &) { return Reply{true, QJsonObject{}, "restored #3 to 100", {}}; });
  rig.fake.hold("undo");
  int first = 0, second = 0;
  QVERIFY(rig.board.run("undo", {}, {}, [&](const Reply &) { ++first; }));
  QVERIFY(!rig.board.run("undo", {}, {}, [&](const Reply &) { ++second; }));
  QCOMPARE(rig.board.line(), QString("moth is busy: undo"));
  QCOMPARE(rig.board.busyText(), QString("moth is busy: undo"));
  QVERIFY(!rig.board.working());
  QTest::qWait(200);
  QCOMPARE(timesSeen(rig.fake, "undo"), 1);
  rig.fake.release("undo");
  QTRY_COMPARE(first, 1);
  QCOMPARE(rig.board.line(), QString("restored #3 to 100"));
  QCOMPARE(rig.board.busyText(), QString());
  // answered: the next one goes
  QVERIFY(rig.board.run("undo", {}, {}, [&](const Reply &) { ++first; }));
  QTRY_COMPARE(first, 2);
  QCOMPARE(timesSeen(rig.fake, "undo"), 2);
  QCOMPARE(second, 0);
}

// the one sentence for why a request is not sent: the user's own words, someone else's request
// as the feed names it, or the service still being started
void TestBoard::busyTextSaysWhatStandsInTheWay() {
  Rig rig;
  QCOMPARE(rig.board.busyText(), QString::fromUtf8("moth is starting…"));  // the start at the board's start
  QVERIFY(rig.ready());
  QCOMPARE(rig.board.busyText(), QString());
  rig.fake.push(busyLine("agent", "find", {"100"}));
  QTRY_VERIFY(rig.board.working());
  QCOMPARE(rig.board.busyText(), QString("moth is busy: find 100 (agent)"));
  QVERIFY(!rig.board.run("next", {"down"}));
  QCOMPARE(rig.board.line(), QString("moth is busy: find 100 (agent)"));
}

// F5: the game closes and the core finds out on the window's own unrecorded status read, which it
// records all the same (R4): the event comes with no busy line before it, and the window ends up
// with no game, no hunt, and the core's sentence in its line
void TestBoard::theGameClosingAsTheCoreRecordsIt() {
  Rig rig;
  rig.fake.on("status", [](const QStringList &, const Opts &) {
    const QJsonObject hunt{{"main", 3}, {"wide", 0}, {"groups", QJsonArray()}, {"blind", false},
                           {"by_tag", QJsonArray{QJsonArray{"whole number", 3}}}, {"can_compare", true}};
    return Reply{true, QJsonObject{{"attached", disco}, {"hunt", hunt}}, discoLine + "\n3 matches", {}};
  });
  QVERIFY(rig.ready());
  QVERIFY(rig.board.hunt().has_value());
  QSignalSpy turned(&rig.board, &Board::turned);
  QList<bool> workingSeen;
  connect(&rig.board, &Board::changed, this, [&] { workingSeen << rig.board.working(); });
  const int statuses = timesSeen(rig.fake, "status");

  rig.fake.closeGame("disco.exe");
  rig.board.tick();
  QTRY_COMPARE(turned.count(), 1);
  QVERIFY(!rig.board.attached().has_value());
  QVERIFY(!rig.board.hunt().has_value());
  QCOMPARE(rig.board.counts(), QString());
  QCOMPARE(rig.board.line(), QString("not attached. disco.exe closed."));
  QVERIFY(!workingSeen.contains(true));  // no busy line came
  QTest::qWait(200);
  // the tick's read and the one the event brought; the line stays the core's sentence
  QCOMPARE(timesSeen(rig.fake, "status"), statuses + 2);
  QCOMPARE(turned.count(), 1);
  QCOMPARE(rig.board.line(), QString("not attached. disco.exe closed."));
  for (const auto &s : rig.fake.seen()) QVERIFY(!s.record);
}

// F7: `moth stop` from elsewhere: its event, then the feed drops. The line says it was stopped,
// not that the service was lost; a drop with no stop before it still says lost
void TestBoard::aStopFromElsewhereIsSaidAsAStop() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.push(eventLine(3, "user", "stop", {}, "stopped"));
  QTRY_COMPARE(rig.board.line(), QString("stopped"));
  rig.fake.stop();
  QTRY_VERIFY(!rig.board.connected());
  QTRY_COMPARE(rig.board.line(), QString("moth's background service was stopped. it starts again with your next action."));
  rig.board.tick();
  QTest::qWait(200);
  QCOMPARE(rig.board.line(), QString("moth's background service was stopped. it starts again with your next action."));

  rig.fake.start();
  QTRY_VERIFY_WITH_TIMEOUT(rig.board.connected(), 2000);
  rig.fake.dropWatchers();
  QTRY_COMPARE(rig.board.line(), lost);
}

QTEST_GUILESS_MAIN(TestBoard)
#include "test_board.moc"
