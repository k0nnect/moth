#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtTest>

#include "fake_core.hpp"
#include "game_bar.hpp"
#include "main_window.hpp"

namespace {

const QString dot = QString::fromUtf8(" · ");

Reply ok(const QJsonValue &result, const QString &text = "") { return Reply{true, result, text, ""}; }
Reply refused(const QString &error) { return Reply{false, QJsonValue(), "", error}; }

QJsonObject statusAttached(const QString &name, const QString &bottle) {
  return {{"attached", QJsonObject{{"pid", 4242}, {"name", name}, {"bits", 64}, {"bottle", bottle}}}, {"hunt", QJsonValue()}};
}

QJsonObject proc(int pid, const QString &name) { return {{"pid", pid}, {"name", name}}; }

// the requests for `cmd` the fake has seen
QList<FakeCore::Seen> seenOf(const FakeCore &fake, const QString &cmd) {
  QList<FakeCore::Seen> out;
  for (const auto &s : fake.seen())
    if (s.cmd == cmd) out.append(s);
  return out;
}

QStringList texts(const QMenu *m) {
  QStringList out;
  for (const QAction *a : m->actions()) out << (a->isSeparator() ? "---" : a->text());
  return out;
}

}  // namespace

class TestGameBar : public QObject {
  Q_OBJECT

private slots:
  void attachSendsAndShowsTheGame();
  void aRefusedAttachChangesNothing();
  void launchWaitsThenAttaches();
  void launchGivesUpAfterTheLimit();
  void aRefusedAttachIsTriedAgain();
  void theWaitingLineDoesNotCoverAnError();
  void bottlesAreLookedInOneAtATime();
  void aSubmenuClosedBeforeItsTurnDoesNotLook();
  void detachSendsAndClears();
  void busyDisablesBothButtons();
  void theMenuListsBottlesAndRememberedGames();
  void aBottleSubmenuListsProgramsOrTheCoresText();
  void aReopenedMenuEndsFilled();
  void aMenuOpenedWhileBusyShowsWhy();
  void itIsFirstInTheWindowsColumn();
};

void TestGameBar::attachSendsAndShowsTheGame() {
  Rig rig;
  QVERIFY(rig.ready());
  GameBar bar(&rig.board);
  auto *label = bar.findChild<QLabel *>("game");
  auto *detach = bar.findChild<QPushButton *>("detach");
  QVERIFY(label && detach);
  QCOMPARE(bar.objectName(), QString("gameBar"));
  QCOMPARE(label->text(), QString("no game attached"));
  QVERIFY(!detach->isEnabled());

  rig.fake.on("attach", [&](const QStringList &, const Opts &) {
    rig.fake.on("status", [](const QStringList &, const Opts &) { return ok(statusAttached("game.exe", "uhmm"), "attached"); });
    return ok(QJsonValue(), "attached to game.exe");
  });
  bar.attach("uhmm", "game.exe");
  QTRY_COMPARE(label->text(), QString("game.exe" + dot + "uhmm" + dot + "64-bit"));
  QTRY_VERIFY(detach->isEnabled());
  const auto sent = seenOf(rig.fake, "attach");
  QCOMPARE(sent.size(), 1);
  QCOMPARE(sent[0].args, QStringList{"game.exe"});
  QCOMPARE(sent[0].opts, (Opts{{"bottle", "uhmm"}}));
  QVERIFY(sent[0].record);
}

void TestGameBar::aRefusedAttachChangesNothing() {
  Rig rig;
  QVERIFY(rig.ready());
  GameBar bar(&rig.board);
  rig.fake.on("attach", [](const QStringList &, const Opts &) { return refused("no program named game.exe in uhmm."); });
  bar.attach("uhmm", "game.exe");
  QTRY_COMPARE(rig.board.line(), QString("no program named game.exe in uhmm."));
  QTest::qWait(100);
  QCOMPARE(bar.findChild<QLabel *>("game")->text(), QString("no game attached"));
  QCOMPARE(rig.board.line(), QString("no program named game.exe in uhmm."));
}

void TestGameBar::launchWaitsThenAttaches() {
  Rig rig;
  QVERIFY(rig.ready());
  GameBar bar(&rig.board);
  bar.setWaiting(20, 2000);
  int polls = 0;
  rig.fake.on("launch", [](const QStringList &, const Opts &) {
    return ok(QJsonObject{{"message", "started"}, {"name", "Game.EXE"}, {"bottle", "uhmm"}, {"path", "/tmp/x/Game.EXE"}});
  });
  rig.fake.on("ps", [&](const QStringList &, const Opts &) {
    return ok(++polls < 2 ? QJsonArray{} : QJsonArray{proc(1, "other.exe"), proc(7, "game.exe")});
  });
  rig.fake.on("attach", [&](const QStringList &, const Opts &) {
    rig.fake.on("status", [](const QStringList &, const Opts &) { return ok(statusAttached("game.exe", "uhmm"), "attached"); });
    return ok(QJsonValue(), "attached to game.exe");
  });

  bar.launch("uhmm", "/tmp/x/Game.EXE");
  QTRY_COMPARE(seenOf(rig.fake, "launch").size(), 1);
  QCOMPARE(seenOf(rig.fake, "launch")[0].args, QStringList{"/tmp/x/Game.EXE"});
  QCOMPARE(seenOf(rig.fake, "launch")[0].opts, (Opts{{"bottle", "uhmm"}}));
  QTRY_COMPARE(seenOf(rig.fake, "attach").size(), 1);
  QVERIFY(polls >= 2);
  QCOMPARE(seenOf(rig.fake, "attach")[0].args, QStringList{"game.exe"});
  QCOMPARE(seenOf(rig.fake, "attach")[0].opts, (Opts{{"bottle", "uhmm"}}));
  for (const auto &s : seenOf(rig.fake, "ps")) { QVERIFY(!s.record); QCOMPARE(s.opts, (Opts{{"bottle", "uhmm"}})); }
  // it stops looking once the board shows the game attached
  QTRY_VERIFY(rig.board.attached().has_value());
  const int after = seenOf(rig.fake, "ps").size();
  QTest::qWait(150);
  QCOMPARE(seenOf(rig.fake, "ps").size(), after);
  QCOMPARE(seenOf(rig.fake, "attach").size(), 1);
}

void TestGameBar::launchGivesUpAfterTheLimit() {
  Rig rig;
  QVERIFY(rig.ready());
  GameBar bar(&rig.board);
  bar.setWaiting(20, 200);
  rig.fake.on("launch", [](const QStringList &, const Opts &) {
    return ok(QJsonObject{{"message", "started"}, {"name", "Game.EXE"}, {"bottle", "uhmm"}, {"path", "/tmp/x/Game.EXE"}});
  });
  rig.fake.on("ps", [](const QStringList &, const Opts &) { return ok(QJsonArray{}, "nothing is running in uhmm."); });
  bar.launch("uhmm", "/tmp/x/Game.EXE");
  QTRY_COMPARE(rig.board.line(), "Game.EXE did not show up in uhmm after 0 seconds. see " + rig.home.path() +
                                     "/launch.log, or pick it once it is running.");
}

// F6: the wait goes on until the board shows the game attached. An attach that is refused (the
// game's modules can't be read yet) leaves its error in the line, which the waiting line does not
// cover, and the next step tries again; one not sent because moth is busy is tried again too
void TestGameBar::aRefusedAttachIsTriedAgain() {
  Rig rig;
  QVERIFY(rig.ready());
  GameBar bar(&rig.board);
  bar.setWaiting(100, 5000);
  rig.fake.on("launch", [](const QStringList &, const Opts &) {
    return ok(QJsonObject{{"message", "started"}, {"name", "game.exe"}, {"bottle", "uhmm"}, {"path", "/tmp/x/game.exe"}});
  });
  rig.fake.on("ps", [](const QStringList &, const Opts &) { return ok(QJsonArray{proc(7, "Game.exe")}); });
  const QString refusal = "couldn't check what's loaded in Game.exe, so moth won't attach. try again.";
  int attaches = 0;
  rig.fake.on("attach", [&](const QStringList &, const Opts &) {
    if (++attaches == 1) return refused(refusal);
    rig.fake.on("status", [](const QStringList &, const Opts &) { return ok(statusAttached("Game.exe", "uhmm"), "attached"); });
    return ok(QJsonValue(), "attached to Game.exe");
  });
  rig.fake.hold("attach");
  bar.launch("uhmm", "/tmp/x/game.exe");
  QTRY_COMPARE(seenOf(rig.fake, "attach").size(), 1);
  QCOMPARE(seenOf(rig.fake, "attach")[0].args, QStringList{"Game.exe"});
  // while that attach is out the steps go on, and send no second one
  QTest::qWait(350);
  QCOMPARE(seenOf(rig.fake, "attach").size(), 1);
  rig.fake.release("attach");
  QTRY_COMPARE(rig.board.line(), refusal);
  QTRY_COMPARE(seenOf(rig.fake, "attach").size(), 2);  // the next step
  QTRY_VERIFY(rig.board.attached().has_value());
  const int after = seenOf(rig.fake, "ps").size();
  QTest::qWait(300);
  QCOMPARE(seenOf(rig.fake, "ps").size(), after);
  QCOMPARE(seenOf(rig.fake, "attach").size(), 2);
}

// F6: the waiting line is said again only over itself or nothing: an error the user's own action
// left is not covered
void TestGameBar::theWaitingLineDoesNotCoverAnError() {
  Rig rig;
  QVERIFY(rig.ready());
  GameBar bar(&rig.board);
  bar.setWaiting(50, 5000);
  rig.fake.on("launch", [](const QStringList &, const Opts &) {
    return ok(QJsonObject{{"message", "started"}, {"name", "Game.EXE"}, {"bottle", "uhmm"}, {"path", "/tmp/x/Game.EXE"}});
  });
  rig.fake.on("ps", [](const QStringList &, const Opts &) { return ok(QJsonArray{}, "nothing is running in uhmm."); });
  rig.fake.on("undo", [](const QStringList &, const Opts &) { return refused("nothing to undo"); });
  bar.launch("uhmm", "/tmp/x/Game.EXE");
  const QString waiting = QString::fromUtf8("waiting for Game.EXE to start…");
  QTRY_COMPARE(rig.board.line(), waiting);
  rig.board.say("");
  QTRY_COMPARE(rig.board.line(), waiting);  // nothing there: said again
  rig.board.run("undo");
  QTRY_COMPARE(rig.board.line(), QString("nothing to undo"));
  QTest::qWait(300);
  QCOMPARE(rig.board.line(), QString("nothing to undo"));
}

// F3: the menu looks in one bottle at a time, and the line says which. A submenu opened while
// another bottle is being looked in says so, and looks once that one is back
void TestGameBar::bottlesAreLookedInOneAtATime() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.on("bottles", [](const QStringList &, const Opts &) { return ok(QJsonArray{"uhmm", "other"}); });
  rig.fake.on("ps", [](const QStringList &, const Opts &o) {
    if (o == Opts{{"bottle", "other"}}) return ok(QJsonArray{}, "nothing is running in other.");
    return ok(QJsonArray{proc(7, "game.exe")});
  });
  GameBar bar(&rig.board);
  bar.fillMenu();
  QTRY_VERIFY(bar.menu()->actions().size() >= 2);
  QMenu *uhmm = bar.menu()->actions().at(0)->menu();
  QMenu *other = bar.menu()->actions().at(1)->menu();
  QVERIFY(uhmm && other);
  rig.board.say("before");

  rig.fake.hold("ps");
  bar.fillBottle(uhmm, "uhmm");
  QCOMPARE(rig.board.line(), QString::fromUtf8("looking in uhmm…"));
  QCOMPARE(texts(uhmm), QStringList{QString::fromUtf8("looking…")});
  QTRY_COMPARE(seenOf(rig.fake, "ps").size(), 1);
  bar.fillBottle(other, "other");
  QCOMPARE(texts(other), QStringList{QString::fromUtf8("looking in uhmm first…")});
  QVERIFY(!other->actions().at(0)->isEnabled());
  QTest::qWait(100);
  QCOMPARE(seenOf(rig.fake, "ps").size(), 1);  // one at a time

  rig.fake.release("ps");
  QTRY_COMPARE(texts(uhmm), QStringList{"game.exe"});
  QTRY_COMPARE(seenOf(rig.fake, "ps").size(), 2);  // then the one still open
  QCOMPARE(seenOf(rig.fake, "ps")[1].opts, (Opts{{"bottle", "other"}}));
  QTRY_COMPARE(texts(other), QStringList{"nothing is running in other."});
  // nothing running: the line says the core's words
  QCOMPARE(rig.board.line(), QString("nothing is running in other."));

  // programs listed: the line goes back to what it said
  rig.board.say("before");
  bar.fillBottle(uhmm, "uhmm");
  QCOMPARE(rig.board.line(), QString::fromUtf8("looking in uhmm…"));
  QTRY_COMPARE(texts(uhmm), QStringList{"game.exe"});
  QCOMPARE(rig.board.line(), QString("before"));
}

// F3: a submenu closed before its turn does not look at all
void TestGameBar::aSubmenuClosedBeforeItsTurnDoesNotLook() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.on("bottles", [](const QStringList &, const Opts &) { return ok(QJsonArray{"uhmm", "other"}); });
  rig.fake.on("ps", [](const QStringList &, const Opts &) { return ok(QJsonArray{proc(7, "game.exe")}); });
  GameBar bar(&rig.board);
  bar.fillMenu();
  QTRY_VERIFY(bar.menu()->actions().size() >= 2);
  QMenu *uhmm = bar.menu()->actions().at(0)->menu();
  QMenu *other = bar.menu()->actions().at(1)->menu();
  rig.fake.hold("ps");
  bar.fillBottle(uhmm, "uhmm");
  bar.fillBottle(other, "other");
  emit other->aboutToHide();  // the pointer moved on
  rig.fake.release("ps");
  QTRY_COMPARE(texts(uhmm), QStringList{"game.exe"});
  QTest::qWait(150);
  QCOMPARE(seenOf(rig.fake, "ps").size(), 1);
}

void TestGameBar::detachSendsAndClears() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.on("status", [](const QStringList &, const Opts &) { return ok(statusAttached("game.exe", "uhmm"), "attached"); });
  rig.board.tick();
  GameBar bar(&rig.board);
  auto *label = bar.findChild<QLabel *>("game");
  QTRY_COMPARE(label->text(), QString("game.exe" + dot + "uhmm" + dot + "64-bit"));
  rig.fake.on("detach", [&](const QStringList &, const Opts &) {
    rig.fake.on("status", [](const QStringList &, const Opts &) {
      return ok(QJsonObject{{"attached", QJsonValue()}, {"hunt", QJsonValue()}}, "not attached");
    });
    return ok(QJsonValue(), "detached");
  });
  bar.detach();
  QTRY_COMPARE(seenOf(rig.fake, "detach").size(), 1);
  QVERIFY(seenOf(rig.fake, "detach")[0].args.isEmpty());
  QTRY_COMPARE(label->text(), QString("no game attached"));
}

void TestGameBar::busyDisablesBothButtons() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.on("status", [](const QStringList &, const Opts &) { return ok(statusAttached("game.exe", "uhmm"), "attached"); });
  rig.board.tick();
  GameBar bar(&rig.board);
  auto *pick = bar.findChild<QPushButton *>("pick");
  auto *detach = bar.findChild<QPushButton *>("detach");
  QVERIFY(pick && detach);
  QCOMPARE(pick->text(), QString("pick a game"));
  QTRY_VERIFY(detach->isEnabled());
  QVERIFY(pick->isEnabled());
  rig.fake.push({{"busy", QJsonObject{{"actor", "agent"}, {"cmd", "find"}, {"args", QJsonArray{"100"}}}}});
  QTRY_VERIFY(!pick->isEnabled());
  QVERIFY(!detach->isEnabled());
  rig.fake.push({{"event", QJsonObject{{"seq", 1}, {"actor", "agent"}, {"cmd", "find"}, {"args", QJsonArray{"100"}},
                                       {"ok", true}, {"summary", "done"}}}});
  QTRY_VERIFY(pick->isEnabled());
  QTRY_VERIFY(detach->isEnabled());
}

void TestGameBar::theMenuListsBottlesAndRememberedGames() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.on("bottles", [](const QStringList &, const Opts &) { return ok(QJsonArray{"uhmm", "other"}); });
  rig.fake.on("games", [](const QStringList &, const Opts &) {
    return ok(QJsonArray{QJsonObject{{"name", "Game.EXE"}, {"bottle", "uhmm"}, {"path", "/tmp/x/Game.EXE"}}});
  });
  GameBar bar(&rig.board);
  bar.fillMenu();
  QTRY_COMPARE(texts(bar.menu()), (QStringList{"uhmm", "other", "---", "Game.EXE" + dot + "uhmm", QString::fromUtf8("another game…")}));
  for (const auto &s : rig.fake.seen()) if (s.cmd == "bottles" || s.cmd == "games") QVERIFY(!s.record);

  rig.fake.on("launch", [](const QStringList &, const Opts &) {
    return ok(QJsonObject{{"name", "Game.EXE"}, {"bottle", "uhmm"}, {"path", "/tmp/x/Game.EXE"}});
  });
  bar.menu()->actions().at(3)->trigger();
  QTRY_COMPARE(seenOf(rig.fake, "launch").size(), 1);
  QCOMPARE(seenOf(rig.fake, "launch")[0].args, QStringList{"/tmp/x/Game.EXE"});
}

void TestGameBar::aBottleSubmenuListsProgramsOrTheCoresText() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.on("bottles", [](const QStringList &, const Opts &) { return ok(QJsonArray{"uhmm", "empty"}); });
  rig.fake.on("ps", [](const QStringList &, const Opts &o) {
    if (o == Opts{{"bottle", "empty"}}) return ok(QJsonArray{}, "nothing is running in empty.");
    return ok(QJsonArray{proc(7, "game.exe")});
  });
  rig.fake.on("attach", [](const QStringList &, const Opts &) { return ok(QJsonValue(), "attached"); });
  GameBar bar(&rig.board);
  bar.fillMenu();
  QTRY_VERIFY(bar.menu()->actions().size() >= 2);
  QMenu *uhmm = bar.menu()->actions().at(0)->menu();
  QMenu *empty = bar.menu()->actions().at(1)->menu();
  QVERIFY(uhmm && empty);
  bar.fillBottle(empty, "empty");
  QTRY_COMPARE(texts(empty), QStringList{"nothing is running in empty."});
  QVERIFY(!empty->actions().at(0)->isEnabled());
  bar.fillBottle(uhmm, "uhmm");
  QTRY_COMPARE(texts(uhmm), QStringList{"game.exe"});
  uhmm->actions().at(0)->trigger();
  QTRY_COMPARE(seenOf(rig.fake, "attach").size(), 1);
  QCOMPARE(seenOf(rig.fake, "attach")[0].args, QStringList{"game.exe"});
  QCOMPARE(seenOf(rig.fake, "attach")[0].opts, (Opts{{"bottle", "uhmm"}}));
}

void TestGameBar::aReopenedMenuEndsFilled() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.on("bottles", [](const QStringList &, const Opts &) { return ok(QJsonArray{"uhmm"}); });
  rig.fake.on("games", [](const QStringList &, const Opts &) { return ok(QJsonArray{}); });
  rig.fake.hold("bottles");
  GameBar bar(&rig.board);
  bar.fillMenu();
  QTRY_COMPARE(timesSeen(rig.fake, "bottles"), 1);
  bar.fillMenu();  // reopened while the first read is out
  QCOMPARE(texts(bar.menu()), QStringList{QString::fromUtf8("looking…")});
  rig.fake.release("bottles");
  QTRY_COMPARE(texts(bar.menu()), (QStringList{"uhmm", "---", QString::fromUtf8("another game…")}));
}

void TestGameBar::aMenuOpenedWhileBusyShowsWhy() {
  Rig rig;
  QVERIFY(rig.ready());
  GameBar bar(&rig.board);
  rig.fake.push({{"busy", QJsonObject{{"actor", "agent"}, {"cmd", "find"}, {"args", QJsonArray{"100"}}}}});
  QTRY_VERIFY(rig.board.working());
  bar.fillMenu();
  QCOMPARE(texts(bar.menu()), QStringList{"moth is busy: find 100 (agent)"});
  QVERIFY(!bar.menu()->actions().at(0)->isEnabled());
  QMenu sub;
  bar.fillBottle(&sub, "uhmm");
  QCOMPARE(texts(&sub), QStringList{"moth is busy: find 100 (agent)"});
}

void TestGameBar::itIsFirstInTheWindowsColumn() {
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  auto *bar = w.findChild<GameBar *>("gameBar");
  QVERIFY(bar);
  QCOMPARE(w.column()->itemAt(0)->widget(), bar);
}

QTEST_MAIN(TestGameBar)
#include "test_game_bar.moc"
