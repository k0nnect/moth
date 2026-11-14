#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtTest>

#include "fake_core.hpp"
#include "main_window.hpp"
#include "search_bar.hpp"

namespace {

Reply ok(const QJsonValue &result, const QString &text = "") { return Reply{true, result, text, ""}; }
Reply refused(const QString &error) { return Reply{false, QJsonValue(), "", error}; }

const QJsonObject disco{{"pid", 4242}, {"name", "disco.exe"}, {"bits", 64}, {"bottle", "uhmm"}};
const QString discoLine = "attached to disco.exe (pid 4242, 64-bit) in bottle uhmm";
const QString counts = "241,178 matches (241,178 whole number)";

QJsonObject huntWith(bool canCompare) {
  return {{"main", 241178}, {"wide", 0}, {"groups", QJsonArray()}, {"blind", false},
          {"by_tag", QJsonArray{QJsonArray{"whole number", 241178}}}, {"can_compare", canCompare}};
}

// the fake's status from now on: attached to disco.exe, with this hunt (null: none)
void statusIs(FakeCore &fake, const QJsonValue &hunt) {
  const QString text = hunt.isObject() ? discoLine + "\n" + counts : discoLine;
  fake.on("status", [hunt, text](const QStringList &, const Opts &) { return ok(QJsonObject{{"attached", disco}, {"hunt", hunt}}, text); });
}

// the board reads status again and has taken in a game, and a hunt or none
bool reads(Rig &rig, bool hunting, bool canCompare = false) {
  rig.board.tick();
  return QTest::qWaitFor([&] {
    const auto &h = rig.board.hunt();
    return rig.board.attached().has_value() && h.has_value() == hunting && (!h || h->canCompare == canCompare);
  });
}

QList<FakeCore::Seen> seenOf(const FakeCore &fake, const QString &cmd) {
  QList<FakeCore::Seen> out;
  for (const auto &s : fake.seen())
    if (s.cmd == cmd) out.append(s);
  return out;
}

QPushButton *button(const SearchBar &bar, const char *name) { return bar.findChild<QPushButton *>(name); }

const char *const everything[] = {"hunt", "narrow", "snapshot", "down", "up", "same", "changed"};

}  // namespace

class TestSearchBar : public QObject {
  Q_OBJECT

private slots:
  void returnStartsAHuntThenNarrows();
  void nothingTypedSendsNothing();
  void whatEachButtonNeeds();
  void theBoxIsEmptiedOnlyWhenItsOwnStepWorked();
  void whatIsTypedBeforeTheReplyStays();
  void aHeldFindDisablesEverything();
  void someoneElsesStepDisablesEverything();
  void anErrorIsTheLineWordForWord();
  void noNumberSendsSnapshot();
  void itIsSecondInTheWindowsColumn();
};

void TestSearchBar::returnStartsAHuntThenNarrows() {
  Rig rig;
  statusIs(rig.fake, QJsonValue());
  QVERIFY(rig.ready());
  QVERIFY(reads(rig, false));
  SearchBar bar(&rig.board);
  QCOMPARE(bar.objectName(), QString("searchBar"));
  auto *number = bar.findChild<QLineEdit *>("number");
  QVERIFY(number);
  QCOMPARE(number->placeholderText(), QString("the number the game shows"));

  rig.fake.on("find", [&](const QStringList &, const Opts &) {
    statusIs(rig.fake, huntWith(true));
    return ok(QJsonObject{{"main", 241178}}, counts);
  });
  QTest::keyClicks(number, " 100 ");
  QTest::keyClick(number, Qt::Key_Return);
  QTRY_COMPARE(seenOf(rig.fake, "find").size(), 1);
  QCOMPARE(seenOf(rig.fake, "find")[0].args, QStringList{"100"});  // as typed, trimmed
  QVERIFY(seenOf(rig.fake, "find")[0].record);
  QTRY_VERIFY(rig.board.hunt().has_value());
  // the step worked: the box is emptied and keeps the focus
  QTRY_VERIFY(number->text().isEmpty());

  rig.fake.on("next", [](const QStringList &, const Opts &) { return ok(QJsonObject{{"main", 3}}, "3 matches"); });
  QTest::keyClicks(number, "90");
  QCOMPARE(number->text(), QString("90"));
  QTest::keyClick(number, Qt::Key_Return);
  QTRY_COMPARE(seenOf(rig.fake, "next").size(), 1);
  QCOMPARE(seenOf(rig.fake, "next")[0].args, QStringList{"90"});
  QCOMPARE(seenOf(rig.fake, "find").size(), 1);
}

void TestSearchBar::nothingTypedSendsNothing() {
  Rig rig;
  statusIs(rig.fake, huntWith(true));
  QVERIFY(rig.ready());
  QVERIFY(reads(rig, true, true));
  SearchBar bar(&rig.board);
  auto *number = bar.findChild<QLineEdit *>("number");
  QTest::keyClicks(number, "   ");
  button(bar, "hunt")->click();
  QCOMPARE(rig.board.line(), QString("type the number the game shows"));
  rig.board.say("");
  button(bar, "narrow")->click();
  QCOMPARE(rig.board.line(), QString("type the number the game shows"));
  rig.board.say("");
  QTest::keyClick(number, Qt::Key_Return);
  QCOMPARE(rig.board.line(), QString("type the number the game shows"));
  QTest::qWait(100);
  QCOMPARE(timesSeen(rig.fake, "find"), 0);
  QCOMPARE(timesSeen(rig.fake, "next"), 0);
}

void TestSearchBar::theBoxIsEmptiedOnlyWhenItsOwnStepWorked() {
  Rig rig;
  statusIs(rig.fake, huntWith(true));
  QVERIFY(rig.ready());
  QVERIFY(reads(rig, true, true));
  SearchBar bar(&rig.board);
  bar.show();
  auto *number = bar.findChild<QLineEdit *>("number");
  rig.fake.on("next", [](const QStringList &args, const Opts &) {
    return args == QStringList{"42"} ? Reply{false, QJsonValue(), "", "no"} : ok(QJsonObject{{"main", 3}}, "3 matches");
  });
  // a refused step keeps what was typed
  number->setFocus();
  QTest::keyClicks(number, "42");
  QTest::keyClick(number, Qt::Key_Return);
  QTRY_COMPARE(seenOf(rig.fake, "next").size(), 1);
  QTRY_COMPARE(rig.board.line(), QString("no"));  // the reply has come
  QTRY_VERIFY(number->isEnabled());
  QCOMPARE(number->text(), QString("42"));
  // a relation button leaves the box alone
  button(bar, "down")->click();
  QTRY_COMPARE(seenOf(rig.fake, "next").size(), 2);
  QTRY_COMPARE(rig.board.line(), QString("3 matches"));
  QTRY_VERIFY(number->isEnabled());
  QCOMPARE(number->text(), QString("42"));
  // a step that works empties it; a second Return sends nothing
  rig.fake.on("next", [](const QStringList &, const Opts &) { return ok(QJsonObject{{"main", 3}}, "3 matches"); });
  QTest::keyClick(number, Qt::Key_Return);
  QTRY_COMPARE(seenOf(rig.fake, "next").size(), 3);
  QTRY_VERIFY(number->text().isEmpty());
  QTRY_VERIFY(number->isEnabled());
  QTRY_VERIFY(number->hasFocus());  // and it keeps the focus
  QTest::keyClick(number, Qt::Key_Return);
  QTest::qWait(100);
  QCOMPARE(seenOf(rig.fake, "next").size(), 3);
}

void TestSearchBar::whatIsTypedBeforeTheReplyStays() {
  Rig rig;
  statusIs(rig.fake, huntWith(true));
  QVERIFY(rig.ready());
  QVERIFY(reads(rig, true, true));
  SearchBar bar(&rig.board);
  bar.show();
  auto *number = bar.findChild<QLineEdit *>("number");
  rig.fake.on("next", [](const QStringList &, const Opts &) { return ok(QJsonObject{{"main", 3}}, "3 matches"); });
  rig.fake.hold("next");
  number->setFocus();
  QTest::keyClicks(number, "42");
  QTest::keyClick(number, Qt::Key_Return);
  QTRY_COMPARE(seenOf(rig.fake, "next").size(), 1);
  number->setText("43");
  rig.fake.release("next");
  QTRY_COMPARE(rig.board.line(), QString("3 matches"));
  QCOMPARE(number->text(), QString("43"));
}

void TestSearchBar::whatEachButtonNeeds() {
  Rig rig;
  QVERIFY(rig.ready());  // no game
  SearchBar bar(&rig.board);
  auto *number = bar.findChild<QLineEdit *>("number");
  auto *countsLabel = bar.findChild<QLabel *>("counts");
  QVERIFY(number && countsLabel);
  QCOMPARE(button(bar, "hunt")->text(), QString("new hunt"));
  QCOMPARE(button(bar, "narrow")->text(), QString("narrow"));
  QCOMPARE(button(bar, "snapshot")->text(), QString("no number"));
  QCOMPARE(button(bar, "down")->text(), QString("went down"));
  QCOMPARE(button(bar, "up")->text(), QString("went up"));
  QCOMPARE(button(bar, "same")->text(), QString("same"));
  QCOMPARE(button(bar, "changed")->text(), QString("changed"));
  QVERIFY(!number->isEnabled());
  for (const char *name : everything) QVERIFY2(!button(bar, name)->isEnabled(), name);

  // a game, no hunt: a hunt can start, nothing can narrow
  statusIs(rig.fake, QJsonValue());
  QVERIFY(reads(rig, false));
  QTRY_VERIFY(number->isEnabled());
  QVERIFY(button(bar, "hunt")->isEnabled());
  QVERIFY(button(bar, "snapshot")->isEnabled());
  QVERIFY(!button(bar, "narrow")->isEnabled());
  for (const char *name : {"down", "up", "same", "changed"}) QVERIFY2(!button(bar, name)->isEnabled(), name);
  QCOMPARE(countsLabel->text(), QString());

  // a hunt that can't be compared by change: a number narrows it, a relation can't
  statusIs(rig.fake, huntWith(false));
  QVERIFY(reads(rig, true, false));
  QTRY_VERIFY(button(bar, "narrow")->isEnabled());
  for (const char *name : {"down", "up", "same", "changed"}) QVERIFY2(!button(bar, name)->isEnabled(), name);
  QCOMPARE(countsLabel->text(), counts);

  statusIs(rig.fake, huntWith(true));
  QVERIFY(reads(rig, true, true));
  for (const char *name : everything) QTRY_VERIFY2(button(bar, name)->isEnabled(), name);

  rig.fake.on("next", [](const QStringList &, const Opts &) { return ok(QJsonObject{{"main", 3}}, "3 matches"); });
  const QList<QPair<const char *, QString>> relations = {{"down", "down"}, {"up", "up"}, {"same", "same"}, {"changed", "changed"}};
  int sent = 0;
  for (const auto &[name, word] : relations) {
    QTRY_VERIFY2(button(bar, name)->isEnabled(), name);
    button(bar, name)->click();
    ++sent;
    QTRY_COMPARE(timesSeen(rig.fake, "next"), sent);
    QCOMPARE(seenOf(rig.fake, "next").last().args, QStringList{word});
  }
}

// Review Focus 1: a click while a scan is running
void TestSearchBar::aHeldFindDisablesEverything() {
  Rig rig;
  statusIs(rig.fake, QJsonValue());
  QVERIFY(rig.ready());
  QVERIFY(reads(rig, false));
  SearchBar bar(&rig.board);
  auto *number = bar.findChild<QLineEdit *>("number");
  QTRY_VERIFY(number->isEnabled());
  rig.fake.on("find", [](const QStringList &, const Opts &) { return ok(QJsonObject{{"main", 3}}, "3 matches"); });
  rig.fake.hold("find");

  QTest::keyClicks(number, "100");
  QTest::keyClick(number, Qt::Key_Return);
  QTest::keyClick(number, Qt::Key_Return);  // again at once, before the service has said it is working
  QCOMPARE(rig.board.line(), QString("moth is busy: find 100"));  // Board::run refuses it, in words
  QTRY_VERIFY(rig.board.working());
  QVERIFY(!number->isEnabled());
  for (const char *name : everything) QVERIFY2(!button(bar, name)->isEnabled(), name);
  QTest::keyClick(number, Qt::Key_Return);
  button(bar, "hunt")->click();
  QTest::qWait(100);
  QCOMPARE(timesSeen(rig.fake, "find"), 1);

  rig.fake.release("find");
  QTRY_VERIFY(button(bar, "hunt")->isEnabled());
  QVERIFY(number->isEnabled());
  QCOMPARE(timesSeen(rig.fake, "find"), 1);
}

void TestSearchBar::someoneElsesStepDisablesEverything() {
  Rig rig;
  statusIs(rig.fake, huntWith(true));
  QVERIFY(rig.ready());
  QVERIFY(reads(rig, true, true));
  SearchBar bar(&rig.board);
  for (const char *name : everything) QTRY_VERIFY2(button(bar, name)->isEnabled(), name);
  rig.fake.push({{"busy", QJsonObject{{"actor", "agent"}, {"cmd", "next"}, {"args", QJsonArray{"down"}}}}});
  QTRY_VERIFY(!button(bar, "hunt")->isEnabled());
  QVERIFY(!bar.findChild<QLineEdit *>("number")->isEnabled());
  for (const char *name : everything) QVERIFY2(!button(bar, name)->isEnabled(), name);
  rig.fake.push({{"event", QJsonObject{{"seq", 1}, {"actor", "agent"}, {"cmd", "next"}, {"args", QJsonArray{"down"}},
                                       {"ok", true}, {"summary", "3 matches"}}}});
  for (const char *name : everything) QTRY_VERIFY2(button(bar, name)->isEnabled(), name);
}

void TestSearchBar::anErrorIsTheLineWordForWord() {
  Rig rig;
  statusIs(rig.fake, huntWith(true));
  QVERIFY(rig.ready());
  QVERIFY(reads(rig, true, true));
  SearchBar bar(&rig.board);
  auto *number = bar.findChild<QLineEdit *>("number");
  QTRY_VERIFY(number->isEnabled());
  const QString error = "'132.5.1' isn't a number";
  rig.fake.on("next", [&](const QStringList &, const Opts &) { return refused(error); });
  QTest::keyClicks(number, "132.5.1");
  button(bar, "narrow")->click();
  QTRY_COMPARE(seenOf(rig.fake, "next").size(), 1);
  QCOMPARE(seenOf(rig.fake, "next")[0].args, QStringList{"132.5.1"});
  QTRY_COMPARE(rig.board.line(), error);
  QTRY_VERIFY(number->isEnabled());
  QCOMPARE(number->text(), QString("132.5.1"));  // left as typed, to be put right
  QVERIFY(!number->hasSelectedText());
}

void TestSearchBar::noNumberSendsSnapshot() {
  Rig rig;
  statusIs(rig.fake, QJsonValue());
  QVERIFY(rig.ready());
  QVERIFY(reads(rig, false));
  SearchBar bar(&rig.board);
  rig.fake.on("snapshot", [](const QStringList &, const Opts &) { return ok(QJsonObject(), "snapshot of 1.2 GB taken."); });
  QTRY_VERIFY(button(bar, "snapshot")->isEnabled());
  button(bar, "snapshot")->click();
  QTRY_COMPARE(seenOf(rig.fake, "snapshot").size(), 1);
  QVERIFY(seenOf(rig.fake, "snapshot")[0].args.isEmpty());
  QVERIFY(seenOf(rig.fake, "snapshot")[0].record);
}

void TestSearchBar::itIsSecondInTheWindowsColumn() {
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setTerminal(false);
  w.setPolling(false);
  auto *bar = w.findChild<SearchBar *>("searchBar");
  QVERIFY(bar);
  QCOMPARE(w.column()->itemAt(1)->widget(), bar);
}

QTEST_MAIN(TestSearchBar)
#include "test_search_bar.moc"
