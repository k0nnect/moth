#include <QCheckBox>
#include <QJsonArray>
#include <QLabel>
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
  QHash<qint64, QString> shown, addr;
  QString shownOf(qint64 n) const { return shown.value(n, QString::number(1000 + n)); }
  QString addrOf(qint64 n) const { return addr.value(n, "0x" + QString::number(0x10000000 + 4 * n, 16)); }
  Reply page(const Opts &opts) const {
    const qint64 from = opt(opts, "from").isEmpty() ? 1 : opt(opts, "from").toLongLong();
    const qint64 n = opt(opts, "n").isEmpty() ? 20 : opt(opts, "n").toLongLong();
    QJsonArray rows;
    for (qint64 i = from; i < from + n && i <= total; ++i)
      rows.append(QJsonObject{{"n", i}, {"addr", addrOf(i)}, {"interp_id", "i32"}, {"tag", "whole number"},
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

// the requests after the first one for `cmd`, and the one just before it
QList<FakeCore::Seen> after(const FakeCore &fake, const QString &cmd) { return fake.seen().mid(firstOf(fake, cmd) + 1); }
FakeCore::Seen justBefore(const FakeCore &fake, const QString &cmd) { return fake.seen().at(firstOf(fake, cmd) - 1); }

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
  void itStopsAtTwentyThousand();
  void aTickRefreshesTheRowsInView();
  void readsThatOverlapEndWithTheList();
  void aReloadWhileBusyWaitsForTheEvent();
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

// Review Focus 2: the row moved between the list being drawn and the user typing over row 3
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
  QCOMPARE(set.opts, (Opts{{"at", was}}));
  QVERIFY(set.record);
  // just before it, row 3 was listed alone, so the core's last listing has it
  const auto before = justBefore(h.rig.fake, "set");
  QCOMPARE(before.cmd, QString("results"));
  QCOMPARE(opt(before.opts, "from"), QString("3"));
  QCOMPARE(opt(before.opts, "n"), QString("1"));
  QVERIFY(!before.record);

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
  h.rig.fake.on("keep", [](const QStringList &, const Opts &) { return ok(QJsonObject{{"message", "kept #2 as hp"}}, "kept #2 as hp"); });
  h.panel->keepSelected("hp");
  QTRY_COMPARE(seenOf(h.rig.fake, "keep").size(), 1);
  const auto keepIt = seenOf(h.rig.fake, "keep")[0];
  QCOMPARE(keepIt.args, (QStringList{"2", "hp"}));
  QCOMPARE(keepIt.opts, (Opts{{"at", h.list.addrOf(2)}}));
  QVERIFY(keepIt.record);
  QCOMPARE(justBefore(h.rig.fake, "keep").cmd, QString("results"));
  QCOMPARE(opt(justBefore(h.rig.fake, "keep").opts, "from"), QString("2"));
  QCOMPARE(opt(justBefore(h.rig.fake, "keep").opts, "n"), QString("1"));
  // the list is read again after it, which forgets the selection
  QTRY_VERIFY(!keep->isEnabled());

  // while moth is working, nothing that would start a request can be used
  QTRY_COMPARE(h.model->rowCount(), 200);
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

void TestResults::theLabelSaysHowMuchIsShown() {
  ListRig h;
  h.list.total = 3;
  QVERIFY(h.start());
  QCOMPARE(h.shown->text(), QString("3 matches"));
  QCOMPARE(h.model->rowCount(), 3);
  QVERIFY(!h.model->canFetchMore({}));
  h.list.total = 1;
  h.model->reload();
  QTRY_COMPARE(h.shown->text(), QString("1 match"));
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
