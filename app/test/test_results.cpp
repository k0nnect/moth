#include <QCheckBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableView>
#include <QVBoxLayout>
#include <QtTest>

#include "fake_core.hpp"
#include "main_window.hpp"
#include "results_model.hpp"
#include "results_panel.hpp"

namespace {

Reply ok(const QJsonValue &result, const QString &text = "") { return Reply{true, result, text, ""}; }
Reply refused(const QString &error) { return Reply{false, QJsonValue(), "", error}; }

const QJsonObject disco{{"pid", 4242}, {"name", "disco.exe"}, {"bits", 64}, {"bottle", "uhmm"}};
const QString discoLine = "attached to disco.exe (pid 4242, 64-bit) in bottle uhmm";

QJsonObject huntOf(bool blind) {
  return {{"main", 241178}, {"wide", 0}, {"groups", QJsonArray()}, {"blind", blind},
          {"by_tag", QJsonArray{QJsonArray{"whole number", 241178}}}, {"can_compare", true}};
}

// the fake's status from now on: attached to disco.exe and hunting (blind: a snapshot no step has narrowed)
void hunting(FakeCore &fake, bool blind = false) {
  fake.on("status", [blind](const QStringList &, const Opts &) {
    return ok(QJsonObject{{"attached", disco}, {"hunt", huntOf(blind)}}, discoLine + "\n241,178 matches");
  });
}

QString opt(const Opts &opts, const QString &key) {
  for (const auto &[k, v] : opts)
    if (k == key) return v;
  return {};
}

bool has(const Opts &opts, const QString &key) {
  for (const auto &[k, v] : opts)
    if (k == key) return true;
  return false;
}

// What the fake lists: `total` rows; row n is at 0x10000000 + 4n and shows 1000 + n, unless
// changed here.
struct Listing {
  qint64 total = 16700000;
  QHash<qint64, QString> shown, addr, kind;
  QString shownOf(qint64 n) const { return shown.value(n, QString::number(1000 + n)); }
  QString addrOf(qint64 n) const { return addr.value(n, "0x" + QString::number(0x10000000 + 4 * n, 16)); }
  QString kindOf(qint64 n) const { return kind.value(n, "i32"); }
  Reply page(const Opts &opts) const {
    const qint64 from = opt(opts, "from").isEmpty() ? 1 : opt(opts, "from").toLongLong();
    const qint64 n = opt(opts, "n").isEmpty() ? 20 : opt(opts, "n").toLongLong();
    QJsonArray rows;
    for (qint64 i = from; i < from + n && i <= total; ++i)
      rows.append(QJsonObject{{"n", i}, {"addr", addrOf(i)}, {"interp_id", kindOf(i)}, {"tag", "whole number"},
                              {"shown", shownOf(i)}, {"away", QJsonValue()}});
    return ok(QJsonObject{{"rows", rows}, {"total", total}, {"from", from}}, "#1 ...");
  }
};

QList<FakeCore::Seen> seenOf(const FakeCore &fake, const QString &cmd) {
  QList<FakeCore::Seen> out;
  for (const auto &s : fake.seen())
    if (s.cmd == cmd) out.append(s);
  return out;
}

// where the first request for `cmd` is among all the fake has seen; -1 if none
qsizetype firstOf(const FakeCore &fake, const QString &cmd) {
  const auto all = fake.seen();
  for (qsizetype i = 0; i < all.size(); ++i)
    if (all[i].cmd == cmd) return i;
  return -1;
}

// the requests after the first one for `cmd`
QList<FakeCore::Seen> after(const FakeCore &fake, const QString &cmd) { return fake.seen().mid(firstOf(fake, cmd) + 1); }

// no `results` read of one row was ever sent: a write is not read first
bool noRowReadAlone(const FakeCore &fake) {
  for (const auto &s : seenOf(fake, "results"))
    if (opt(s.opts, "n") == "1") return false;
  return true;
}

int count(const QList<FakeCore::Seen> &seen, const QString &cmd) {
  int n = 0;
  for (const auto &s : seen)
    if (s.cmd == cmd) ++n;
  return n;
}

QJsonObject eventLine(qint64 seq, const QString &cmd, const QString &summary) {
  return {{"event", QJsonObject{{"seq", seq}, {"actor", "agent"}, {"cmd", cmd}, {"args", QJsonArray()}, {"ok", true}, {"summary", summary}}}};
}

QString cell(const ResultsModel *model, int row, int column) { return model->data(model->index(row, column)).toString(); }

// a rig hunting, its fake listing `list`, and a results panel whose first page has come
struct ListRig {
  Listing list;
  Rig rig;
  std::unique_ptr<ResultsPanel> panel;
  ResultsModel *model = nullptr;
  QTableView *view = nullptr;
  QLabel *shown = nullptr;
  bool start(bool blind = false) {
    hunting(rig.fake, blind);
    rig.fake.on("results", [this](const QStringList &, const Opts &o) { return list.page(o); });
    if (!rig.ready() || !QTest::qWaitFor([this] { return rig.board.hunt().has_value(); })) return false;
    panel = std::make_unique<ResultsPanel>(&rig.board);
    model = panel->model();
    view = panel->findChild<QTableView *>("results");
    shown = panel->findChild<QLabel *>("shown");
    return model && view && shown && (blind || QTest::qWaitFor([this] { return model->rowCount() == std::min<qint64>(list.total, 200); }));
  }
};

}  // namespace

class TestResults : public QObject {
  Q_OBJECT

private slots:
  void sixteenMillionResultsComeAPageAtATime();
  void aRowShowsWhatTheCoreListed();
  void aRowThatMovedIsNotWrittenAndTheListIsReadAgain();
  void aWriteTheCoreTookShowsInTheCell();
  void keepSelectedKeepsThatRowAtItsAddress();
  void refreshChangesOnlyTheCellsThatChanged();
  void aRowAtAnotherAddressReloads();
  void theGameClosingEmptiesTheList();
  void aSnapshotListsNothingYet();
  void showRarerFormsAddsAll();
  void theLabelSaysHowMuchIsShown();
  void theKindColumnFitsItsWidestWord();
  void itStopsAtTwentyThousand();
  void aTickRefreshesTheRowsInView();
  void readsThatOverlapEndWithTheList();
  void aReloadWhileBusyWaitsForTheEvent();
  void aRefusedKeepOrSetSaysTheCoresWordsAndReadsTheListAgain();
  void aWriteWhileBusyIsNotSentAndLeavesTheList();
  void aWriteReadsNothingFirstAndPollingGoesOn();
  void aWriteNamesItsKindAndTheRarerForms();
  void aSetThatWorkedKeepsTheListAndItsSelection();
  void anEventWithAnotherTotalStartsTheListAgain();
  void aRefreshWhileEditingLeavesTheEditorAlone();
  void itIsThirdInTheWindowsColumn();
};

// Review Focus 5: sixteen million results
void TestResults::sixteenMillionResultsComeAPageAtATime() {
  ListRig h;
  QVERIFY(h.start());  // the first page comes at once: the window opened mid-hunt
  QCOMPARE(h.panel->objectName(), QString("resultsPanel"));
  const int before = timesSeen(h.rig.fake, "results");
  h.model->reload();
  QCOMPARE(h.model->rowCount(), 0);  // it forgets its rows
  QTRY_COMPARE(h.model->rowCount(), 200);
  QTest::qWait(100);
  QCOMPARE(timesSeen(h.rig.fake, "results"), before + 1);
  const auto read = seenOf(h.rig.fake, "results").last();
  QCOMPARE(opt(read.opts, "n"), QString("200"));
  QVERIFY(opt(read.opts, "from").isEmpty() || opt(read.opts, "from") == "1");
  QVERIFY(!has(read.opts, "all"));
  QVERIFY(!read.record);
  QCOMPARE(h.model->total(), qint64(16700000));
  QCOMPARE(h.shown->text(), QString("showing 200 of 16,700,000"));

  QVERIFY(h.model->canFetchMore({}));
  h.model->fetchMore({});
  QTRY_COMPARE(h.model->rowCount(), 400);
  QCOMPARE(timesSeen(h.rig.fake, "results"), before + 2);
  const auto more = seenOf(h.rig.fake, "results").last();
  QCOMPARE(opt(more.opts, "from"), QString("201"));
  QCOMPARE(opt(more.opts, "n"), QString("200"));
  QVERIFY(!more.record);
  QCOMPARE(h.shown->text(), QString("showing 400 of 16,700,000"));
  QCOMPARE(cell(h.model, 399, 0), QString("400"));
}

void TestResults::aRowShowsWhatTheCoreListed() {
  ListRig h;
  h.list.shown[2] = "2.5";
  QVERIFY(h.start());
  QCOMPARE(h.model->columnCount(), 4);
  QStringList heads;
  for (int c = 0; c < 4; ++c) heads << h.model->headerData(c, Qt::Horizontal).toString();
  QCOMPARE(heads, (QStringList{"#", "value", "kind", "address"}));
  QCOMPARE(cell(h.model, 1, 0), QString("2"));
  QCOMPARE(cell(h.model, 1, 1), QString("2.5"));
  QCOMPARE(cell(h.model, 1, 2), QString("whole number"));
  QCOMPARE(cell(h.model, 1, 3), h.list.addrOf(2));
  // the value, and only the value, can be typed over
  for (int c = 0; c < 4; ++c) QCOMPARE(bool(h.model->flags(h.model->index(1, c)) & Qt::ItemIsEditable), c == 1);
}

// Review Focus 2: the row moved between the list being drawn and the user typing over row 3. The
// write names the row's address and kind; the core finds it elsewhere and refuses, in its words
void TestResults::aRowThatMovedIsNotWrittenAndTheListIsReadAgain() {
  ListRig h;
  QVERIFY(h.start());
  const QString was = h.list.addrOf(3);
  const QString moved = "row 3 is not at " + was + " any more: the list changed. look again: moth results";
  h.rig.fake.on("set", [&](const QStringList &, const Opts &) {
    h.list.addr[3] = "0x20000000";  // somebody narrowed: row 3 is somewhere else now
    h.list.shown[3] = "77";
    return refused(moved);
  });
  QVERIFY(h.model->setData(h.model->index(2, 1), "9"));
  QTRY_COMPARE(seenOf(h.rig.fake, "set").size(), 1);
  const auto set = seenOf(h.rig.fake, "set")[0];
  QCOMPARE(set.args, (QStringList{"3", "9"}));
  QCOMPARE(set.opts, (Opts{{"at", was}, {"kind", "i32"}}));
  QVERIFY(set.record);
  QVERIFY(noRowReadAlone(h.rig.fake));  // the row is not read first: the core finds it itself

  QTRY_COMPARE(h.rig.board.line(), moved);
  QTRY_COMPARE(cell(h.model, 2, 1), QString("77"));
  QCOMPARE(cell(h.model, 2, 3), QString("0x20000000"));
  QTest::qWait(100);
  QCOMPARE(count(after(h.rig.fake, "set"), "results"), 1);
  QCOMPARE(opt(after(h.rig.fake, "set").last().opts, "from"), QString("1"));
  QCOMPARE(h.rig.board.line(), moved);
}

void TestResults::aWriteTheCoreTookShowsInTheCell() {
  ListRig h;
  QVERIFY(h.start());
  h.rig.fake.on("set", [&](const QStringList &, const Opts &) {
    h.list.shown[3] = "9";
    return ok(QJsonObject{{"message", "set #3 to 9"}, {"held", true}}, "set #3 to 9");
  });
  // typing the number it already shows writes nothing
  QVERIFY(!h.model->setData(h.model->index(2, 1), " 1003 "));
  QTest::qWait(50);
  QCOMPARE(timesSeen(h.rig.fake, "set"), 0);

  h.rig.fake.hold("set");
  QVERIFY(h.model->setData(h.model->index(2, 1), "9"));
  QTRY_COMPARE(timesSeen(h.rig.fake, "set"), 1);
  QTest::qWait(50);
  QCOMPARE(cell(h.model, 2, 1), QString("1003"));  // not until the core says it happened
  h.rig.fake.release("set");
  QTRY_COMPARE(cell(h.model, 2, 1), QString("9"));
}

void TestResults::keepSelectedKeepsThatRowAtItsAddress() {
  ListRig h;
  QVERIFY(h.start());
  auto *keep = h.panel->findChild<QPushButton *>("keep");
  QVERIFY(keep);
  QCOMPARE(keep->text(), QString("keep"));
  QVERIFY(!keep->isEnabled());
  h.view->selectRow(1);
  QVERIFY(keep->isEnabled());
  QSignalSpy reset(h.model, &QAbstractItemModel::modelReset);
  h.rig.fake.on("keep", [](const QStringList &, const Opts &) { return ok(QJsonObject{{"message", "kept #2 as hp"}}, "kept #2 as hp"); });
  h.panel->keepSelected("hp");
  QVERIFY(!keep->isEnabled());  // one write at a time
  QTRY_COMPARE(seenOf(h.rig.fake, "keep").size(), 1);
  const auto keepIt = seenOf(h.rig.fake, "keep")[0];
  QCOMPARE(keepIt.args, (QStringList{"2", "hp"}));
  QCOMPARE(keepIt.opts, (Opts{{"at", h.list.addrOf(2)}, {"kind", "i32"}}));
  QVERIFY(keepIt.record);
  QVERIFY(noRowReadAlone(h.rig.fake));
  // R14: its event leaves the list the same, so the row stays selected
  QTRY_COMPARE(count(after(h.rig.fake, "keep"), "results"), 1);
  QTRY_VERIFY(keep->isEnabled());
  QCOMPARE(h.view->selectionModel()->selectedRows(), QModelIndexList{h.model->index(1, 0)});
  QCOMPARE(reset.count(), 0);

  // while moth is working, nothing that would start a request can be used
  h.view->selectRow(4);
  QVERIFY(keep->isEnabled());
  h.rig.fake.push({{"busy", QJsonObject{{"actor", "agent"}, {"cmd", "next"}, {"args", QJsonArray{"down"}}}}});
  QTRY_VERIFY(!keep->isEnabled());
  QVERIFY(!(h.model->flags(h.model->index(4, 1)) & Qt::ItemIsEditable));
}

void TestResults::refreshChangesOnlyTheCellsThatChanged() {
  ListRig h;
  QVERIFY(h.start());
  h.view->selectRow(7);
  QSignalSpy changed(h.model, &QAbstractItemModel::dataChanged);
  QSignalSpy reset(h.model, &QAbstractItemModel::modelReset);
  h.list.shown[5] = "5555";
  const int before = timesSeen(h.rig.fake, "results");
  h.model->refresh(0, 10);
  QTRY_COMPARE(changed.count(), 1);
  QTest::qWait(100);
  QCOMPARE(changed.count(), 1);
  QCOMPARE(changed[0][0].value<QModelIndex>(), h.model->index(4, 1));
  QCOMPARE(changed[0][1].value<QModelIndex>(), h.model->index(4, 1));
  QCOMPARE(cell(h.model, 4, 1), QString("5555"));
  QCOMPARE(reset.count(), 0);
  QCOMPARE(h.model->rowCount(), 200);
  QCOMPARE(h.view->selectionModel()->selectedRows(), QModelIndexList{h.model->index(7, 0)});
  QCOMPARE(timesSeen(h.rig.fake, "results"), before + 1);
  QCOMPARE(opt(seenOf(h.rig.fake, "results").last().opts, "from"), QString("1"));
}

void TestResults::aRowAtAnotherAddressReloads() {
  ListRig h;
  QVERIFY(h.start());
  QSignalSpy reset(h.model, &QAbstractItemModel::modelReset);
  h.list.addr[6] = "0x30000000";
  h.model->refresh(0, 10);
  QTRY_COMPARE(reset.count(), 1);
  QTRY_COMPARE(h.model->rowCount(), 200);
  QCOMPARE(cell(h.model, 5, 3), QString("0x30000000"));
}

// Review Focus 3: the game closes mid-hunt
void TestResults::theGameClosingEmptiesTheList() {
  ListRig h;
  QVERIFY(h.start());
  QVERIFY(!h.shown->text().isEmpty());
  h.rig.fake.on("status", [](const QStringList &, const Opts &) {
    return ok(QJsonObject{{"attached", QJsonValue()}, {"hunt", QJsonValue()}, {"closed", "disco.exe"}}, "not attached. disco.exe closed.");
  });
  QSignalSpy turned(&h.rig.board, &Board::turned);
  const int before = timesSeen(h.rig.fake, "results");
  h.rig.fake.push(eventLine(1, "status", "not attached. disco.exe closed."));
  QTRY_COMPARE(turned.count(), 1);
  QCOMPARE(h.model->rowCount(), 0);
  QCOMPARE(h.shown->text(), QString());
  QTest::qWait(100);
  QCOMPARE(timesSeen(h.rig.fake, "results"), before);
}

void TestResults::aSnapshotListsNothingYet() {
  ListRig h;
  QVERIFY(h.start(true));
  QTRY_COMPARE(h.shown->text(), QString("nothing to list yet"));
  QSignalSpy turned(&h.rig.board, &Board::turned);
  h.rig.fake.push(eventLine(1, "snapshot", "snapshot of 1.2 GB taken."));
  QTRY_COMPARE(turned.count(), 1);
  h.rig.board.tick();
  QTest::qWait(100);
  QCOMPARE(h.model->rowCount(), 0);
  QCOMPARE(timesSeen(h.rig.fake, "results"), 0);
  QCOMPARE(h.shown->text(), QString("nothing to list yet"));
}

void TestResults::showRarerFormsAddsAll() {
  ListRig h;
  QVERIFY(h.start());
  auto *all = h.panel->findChild<QCheckBox *>("all");
  QVERIFY(all);
  QCOMPARE(all->text(), QString("show rarer forms"));
  const int before = timesSeen(h.rig.fake, "results");
  all->click();
  QTRY_COMPARE(timesSeen(h.rig.fake, "results"), before + 1);
  const auto read = seenOf(h.rig.fake, "results").last();
  QCOMPARE(opt(read.opts, "all"), QString("true"));
  QCOMPARE(opt(read.opts, "n"), QString("200"));
  QVERIFY(opt(read.opts, "from").isEmpty() || opt(read.opts, "from") == "1");
  QTRY_COMPARE(h.model->rowCount(), 200);
  h.model->fetchMore({});
  QTRY_COMPARE(h.model->rowCount(), 400);
  QCOMPARE(opt(seenOf(h.rig.fake, "results").last().opts, "all"), QString("true"));
}

void TestResults::theKindColumnFitsItsWidestWord() {
  ListRig h;
  h.list.total = 48;
  QVERIFY(h.start());
  QVERIFY(h.view->columnWidth(ResultsModel::Kind) >= h.view->fontMetrics().horizontalAdvance("whole number (8-byte)"));
  QVERIFY(h.view->columnWidth(ResultsModel::Number) >= h.view->fontMetrics().horizontalAdvance("999999"));
  QCOMPARE(h.shown->text(), QString());  // 48 of 48: nothing to say
}

void TestResults::theLabelSaysHowMuchIsShown() {
  ListRig h;
  h.list.total = 3;
  QVERIFY(h.start());
  QCOMPARE(h.shown->text(), QString());  // all loaded: the count is in the search bar
  QCOMPARE(h.model->rowCount(), 3);
  QVERIFY(!h.model->canFetchMore({}));
  h.list.total = 1;
  h.model->reload();
  QTRY_COMPARE(h.shown->text(), QString());
  h.list.total = 1234567;
  h.model->reload();
  QTRY_COMPARE(h.shown->text(), QString("showing 200 of 1,234,567"));
}

void TestResults::itStopsAtTwentyThousand() {
  ListRig h;
  h.list.total = 20500;
  QVERIFY(h.start());
  while (h.model->canFetchMore({})) {
    const int rows = h.model->rowCount();
    h.model->fetchMore({});
    QVERIFY(QTest::qWaitFor([&] { return h.model->rowCount() > rows; }));
  }
  QCOMPARE(h.model->rowCount(), 20000);
  QCOMPARE(timesSeen(h.rig.fake, "results"), 100);
  QCOMPARE(h.shown->text(), QString("showing the first 20,000 of 20,500. narrow the hunt to see fewer."));
}

void TestResults::aTickRefreshesTheRowsInView() {
  ListRig h;
  QVERIFY(h.start());
  h.view->resize(400, 300);
  h.list.shown[2] = "4242";
  const int before = timesSeen(h.rig.fake, "results");
  h.rig.board.tick();
  QTRY_COMPARE(cell(h.model, 1, 1), QString("4242"));
  QCOMPARE(timesSeen(h.rig.fake, "results"), before + 1);
  QCOMPARE(opt(seenOf(h.rig.fake, "results").last().opts, "from"), QString("1"));
}

// Reads for the same page can overlap: a reload while the last one is out, rarer forms on and off.
// The list ends with the page it asked for last, and is never left waiting.
void TestResults::readsThatOverlapEndWithTheList() {
  ListRig h;
  QVERIFY(h.start());
  const int before = timesSeen(h.rig.fake, "results");
  h.rig.fake.hold("results");
  h.model->reload();
  QTRY_COMPARE(timesSeen(h.rig.fake, "results"), before + 1);
  h.model->reload();  // the same read is out: it is not sent again, and its reply will do
  h.model->setAll(true);
  h.model->setAll(false);
  QTRY_COMPARE(timesSeen(h.rig.fake, "results"), before + 2);  // the `all` one
  QCOMPARE(h.model->rowCount(), 0);
  h.rig.fake.release("results");
  QTRY_COMPARE(h.model->rowCount(), 200);
  QTest::qWait(100);
  QCOMPARE(h.model->rowCount(), 200);
  QCOMPARE(h.shown->text(), QString("showing 200 of 16,700,000"));
  QCOMPARE(timesSeen(h.rig.fake, "results"), before + 2);
}

// R11: a read that was not sent shows no waiting; the event that ends the work brings the list
void TestResults::aReloadWhileBusyWaitsForTheEvent() {
  ListRig h;
  QVERIFY(h.start());
  h.rig.fake.push({{"busy", QJsonObject{{"actor", "agent"}, {"cmd", "next"}, {"args", QJsonArray{"down"}}}}});
  QTRY_VERIFY(h.rig.board.working());
  const int before = timesSeen(h.rig.fake, "results");
  h.model->reload();
  QCOMPARE(h.model->rowCount(), 0);
  QCOMPARE(h.shown->text(), QString());
  QTest::qWait(100);
  QCOMPARE(timesSeen(h.rig.fake, "results"), before);
  h.rig.fake.push(eventLine(1, "next", "3 matches"));
  QTRY_COMPARE(h.model->rowCount(), 200);
  QCOMPARE(h.shown->text(), QString("showing 200 of 16,700,000"));
}

// R1: whatever the core refuses a row's keep or set with is the line, word for word, and the list
// is read again from the start
void TestResults::aRefusedKeepOrSetSaysTheCoresWordsAndReadsTheListAgain() {
  ListRig h;
  QVERIFY(h.start());
  const QString moved = "row 3 is not at " + h.list.addrOf(3) + " any more: the list changed. look again: moth results";
  h.rig.fake.on("keep", [&](const QStringList &, const Opts &) {
    h.list.addr[3] = "0x30000000";
    return refused(moved);
  });
  QSignalSpy reset(h.model, &QAbstractItemModel::modelReset);
  h.view->selectRow(2);
  h.panel->keepSelected("hp");
  QTRY_COMPARE(h.rig.board.line(), moved);
  QTRY_COMPARE(reset.count(), 1);
  QTRY_COMPARE(cell(h.model, 2, 3), QString("0x30000000"));
  QTRY_VERIFY(!h.model->writing());

  const QString closed = "the game closed. attach again once it's running.";
  h.rig.fake.on("set", [&](const QStringList &, const Opts &) { return refused(closed); });
  QVERIFY(h.model->setData(h.model->index(2, 1), "9"));
  QTRY_COMPARE(h.rig.board.line(), closed);
  QTRY_COMPARE(reset.count(), 2);
  QTRY_COMPARE(h.model->rowCount(), 200);
  QVERIFY(noRowReadAlone(h.rig.fake));
}

// F4: a write while moth is working is not sent: the line says what is running, and the list is
// left as it is (no reload); the event that ends the work checks it as any event does
void TestResults::aWriteWhileBusyIsNotSentAndLeavesTheList() {
  ListRig h;
  QVERIFY(h.start());
  QSignalSpy reset(h.model, &QAbstractItemModel::modelReset);
  h.rig.fake.push({{"busy", QJsonObject{{"actor", "agent"}, {"cmd", "next"}, {"args", QJsonArray{"down"}}}}});
  QTRY_VERIFY(h.rig.board.working());
  const int before = timesSeen(h.rig.fake, "results");
  QVERIFY(!h.model->setData(h.model->index(2, 1), "9"));
  QCOMPARE(h.rig.board.line(), QString("moth is busy: next down (agent)"));
  QVERIFY(!h.model->writing());
  QCOMPARE(h.model->rowCount(), 200);
  QCOMPARE(cell(h.model, 2, 1), QString("1003"));
  QTest::qWait(100);
  QCOMPARE(timesSeen(h.rig.fake, "results"), before);
  QCOMPARE(timesSeen(h.rig.fake, "set"), 0);
  h.rig.fake.push(eventLine(1, "next", "3 matches"));
  QTRY_VERIFY(timesSeen(h.rig.fake, "results") > before);
  QTest::qWait(100);
  QCOMPARE(h.model->rowCount(), 200);
  QCOMPARE(reset.count(), 0);
  QCOMPARE(timesSeen(h.rig.fake, "set"), 0);
}

// F1: a write is sent at once, with nothing read first, and the list's own reads go on while it is
// out (they can no longer move the row the core writes). A second write before the first is
// answered is refused by Board::run, in words
void TestResults::aWriteReadsNothingFirstAndPollingGoesOn() {
  ListRig h;
  QVERIFY(h.start());
  h.rig.fake.feedRequests(false);  // no busy line: the board would drop reads by itself
  h.rig.fake.on("set", [&](const QStringList &, const Opts &) {
    h.list.shown[3] = "9";
    return ok(QJsonObject{{"message", "set #3 to 9"}, {"held", true}}, "set #3 to 9");
  });
  h.rig.fake.hold("set");
  QSignalSpy reset(h.model, &QAbstractItemModel::modelReset);
  const int before = timesSeen(h.rig.fake, "results");
  QVERIFY(h.model->setData(h.model->index(2, 1), "9"));
  QTRY_COMPARE(timesSeen(h.rig.fake, "set"), 1);
  QCOMPARE(timesSeen(h.rig.fake, "results"), before);
  QVERIFY(h.model->writing());

  h.model->refresh(0, 10);
  h.model->fetchMore({});
  QTRY_COMPARE(timesSeen(h.rig.fake, "results"), before + 2);  // page 1 again, and page 2
  QTRY_COMPARE(h.model->rowCount(), 400);

  QVERIFY(!h.model->setData(h.model->index(4, 1), "7"));
  QCOMPARE(h.rig.board.line(), QString("moth is busy: set 3 9"));
  QTest::qWait(100);
  QCOMPARE(timesSeen(h.rig.fake, "set"), 1);

  h.rig.fake.release("set");
  QTRY_COMPARE(cell(h.model, 2, 1), QString("9"));
  QTRY_VERIFY(!h.model->writing());
  QCOMPARE(h.rig.board.line(), QString("set #3 to 9"));
  QCOMPARE(reset.count(), 0);
  QVERIFY(noRowReadAlone(h.rig.fake));
}

// F1: the write names the kind the row showed (its interp_id), and `all` while the rarer forms are
// shown, so the core numbers its rows as this list does
void TestResults::aWriteNamesItsKindAndTheRarerForms() {
  ListRig h;
  h.list.kind[2] = "i16";
  QVERIFY(h.start());
  h.rig.fake.on("set", [](const QStringList &, const Opts &) { return ok(QJsonObject{{"message", "set"}}, "set"); });
  h.rig.fake.on("keep", [](const QStringList &, const Opts &) { return ok(QJsonObject{{"message", "kept"}}, "kept"); });
  QVERIFY(h.model->setData(h.model->index(1, 1), "9"));
  QTRY_COMPARE(timesSeen(h.rig.fake, "set"), 1);
  QCOMPARE(seenOf(h.rig.fake, "set")[0].opts, (Opts{{"at", h.list.addrOf(2)}, {"kind", "i16"}}));
  QTRY_VERIFY(!h.model->writing());

  h.model->setAll(true);
  QTRY_COMPARE(h.model->rowCount(), 200);
  QTRY_VERIFY(h.model->listed());
  QVERIFY(h.model->setData(h.model->index(4, 1), "9"));
  QTRY_COMPARE(timesSeen(h.rig.fake, "set"), 2);
  const auto set = seenOf(h.rig.fake, "set")[1];
  QCOMPARE(set.args, (QStringList{"5", "9"}));
  QCOMPARE(set.opts, (Opts{{"all", "true"}, {"at", h.list.addrOf(5)}, {"kind", "i32"}}));
  QTRY_VERIFY(!h.model->writing());
  h.model->keep(4, "");
  QTRY_COMPARE(timesSeen(h.rig.fake, "keep"), 1);
  const auto keepIt = seenOf(h.rig.fake, "keep")[0];
  QCOMPARE(keepIt.args, (QStringList{"5"}));
  QCOMPARE(keepIt.opts, (Opts{{"all", "true"}, {"at", h.list.addrOf(5)}, {"kind", "i32"}}));
}

// R14: an event that leaves the list the same keeps it, its selection and its place
void TestResults::aSetThatWorkedKeepsTheListAndItsSelection() {
  ListRig h;
  QVERIFY(h.start());
  h.view->selectRow(2);
  QSignalSpy reset(h.model, &QAbstractItemModel::modelReset);
  h.rig.fake.on("set", [&](const QStringList &, const Opts &) {
    h.list.shown[3] = "9";
    return ok(QJsonObject{{"message", "set #3 to 9"}, {"held", true}}, "set #3 to 9");
  });
  QVERIFY(h.model->setData(h.model->index(2, 1), "9"));
  QTRY_COMPARE(timesSeen(h.rig.fake, "set"), 1);
  QTRY_COMPARE(count(after(h.rig.fake, "set"), "results"), 1);  // the event's check of the list
  QTRY_COMPARE(cell(h.model, 2, 1), QString("9"));
  QTRY_VERIFY(!h.model->writing());
  QTest::qWait(100);
  QCOMPARE(reset.count(), 0);
  QCOMPARE(h.model->rowCount(), 200);
  QCOMPARE(h.view->selectionModel()->selectedRows(), QModelIndexList{h.model->index(2, 0)});
  QCOMPARE(h.rig.board.line(), QString("set #3 to 9"));
}

// R14: an event after which the core lists another number of rows: the list starts again
void TestResults::anEventWithAnotherTotalStartsTheListAgain() {
  ListRig h;
  QVERIFY(h.start());
  h.view->selectRow(2);
  QSignalSpy reset(h.model, &QAbstractItemModel::modelReset);
  h.list.total = 5000;
  h.rig.fake.push(eventLine(1, "next", "5,000 matches"));
  QTRY_COMPARE(reset.count(), 1);
  QTRY_COMPARE(h.model->total(), qint64(5000));
  QTRY_COMPARE(h.model->rowCount(), 200);
  QVERIFY(!h.view->selectionModel()->hasSelection());
  QCOMPARE(h.shown->text(), QString("showing 200 of 5,000"));
}

// F2: a value being typed in is not replaced when the poll reads its page again, though the game
// changed it meanwhile; once editing is over, the next read shows the game's value
void TestResults::aRefreshWhileEditingLeavesTheEditorAlone() {
  ListRig h;
  QVERIFY(h.start());
  h.panel->resize(500, 400);
  h.panel->show();
  const QModelIndex value = h.model->index(2, ResultsModel::Value);
  h.view->setCurrentIndex(value);
  h.view->edit(value);
  auto *e = h.view->viewport()->findChild<QLineEdit *>();
  QVERIFY(e);
  QCOMPARE(e->text(), QString("1003"));
  e->setText("77");          // typed, not yet Return
  h.list.shown[3] = "250";   // the game moved it meanwhile
  h.list.shown[2] = "4242";  // and another value in view
  const int before = timesSeen(h.rig.fake, "results");
  h.rig.board.tick();
  QTRY_COMPARE(cell(h.model, 1, 1), QString("4242"));  // its page was read again
  QVERIFY(timesSeen(h.rig.fake, "results") > before);
  QCOMPARE(h.view->viewport()->findChild<QLineEdit *>(), e);
  QCOMPARE(e->text(), QString("77"));
  QCOMPARE(cell(h.model, 2, 1), QString("1003"));  // not replaced under the editor
  QTest::keyClick(e, Qt::Key_Escape);
  h.rig.board.tick();
  QTRY_COMPARE(cell(h.model, 2, 1), QString("250"));
  QCOMPARE(timesSeen(h.rig.fake, "set"), 0);
}

void TestResults::itIsThirdInTheWindowsColumn() {
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  auto *panel = w.findChild<ResultsPanel *>("resultsPanel");
  QVERIFY(panel);
  QCOMPARE(w.column()->itemAt(2)->widget(), panel);
  QVERIFY(w.findChild<QTableView *>("results"));
}

QTEST_MAIN(TestResults)
#include "test_results.moc"
