#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>
#include <QtTest>

#include "fake_core.hpp"
#include "kept_panel.hpp"
#include "main_window.hpp"

namespace {

Reply ok(const QJsonValue &result, const QString &text = "") { return Reply{true, result, text, ""}; }
Reply refused(const QString &error) { return Reply{false, QJsonValue(), "", error}; }

const QJsonObject disco{{"pid", 4242}, {"name", "disco.exe"}, {"bits", 64}, {"bottle", "uhmm"}};
const QString discoLine = "attached to disco.exe (pid 4242, 64-bit) in bottle uhmm";

QJsonObject item(const QString &name, const QString &addr, const QString &tag, const QString &shown,
                 const QString &note = "") {
  return {{"name", name}, {"addr", addr}, {"tag", tag}, {"shown", shown}, {"set_to", QJsonValue()},
          {"put_back", false}, {"note", note}};
}

// a rig attached to disco.exe whose fake keeps `points` and `xp`, and a kept panel
struct KeptRig {
  Rig rig;
  QJsonArray kept{item("points", "0x6c551000", "whole number", "10"),
                  item("xp", "0x6c551068", "whole number", "92", "held at 95")};
  std::unique_ptr<KeptPanel> panel;
  QTableView *view = nullptr;
  QPushButton *forget = nullptr, *undo = nullptr;
  QLabel *nothing = nullptr;
  bool start() {
    rig.fake.on("status", [](const QStringList &, const Opts &) {
      return ok(QJsonObject{{"attached", disco}, {"hunt", QJsonValue()}}, discoLine);
    });
    rig.fake.on("kept", [this](const QStringList &, const Opts &) { return ok(kept); });
    if (!rig.ready() || !QTest::qWaitFor([this] { return rig.board.attached().has_value(); })) return false;
    return build();
  }
  bool build() {
    panel = std::make_unique<KeptPanel>(&rig.board);
    panel->show();
    view = panel->findChild<QTableView *>("kept");
    forget = panel->findChild<QPushButton *>("forget");
    undo = panel->findChild<QPushButton *>("undo");
    nothing = panel->findChild<QLabel *>("nothing");
    return view && forget && undo && nothing;
  }
  QString cell(int row, int col) const { return view->model()->index(row, col).data().toString(); }
  QString shownOf(const QString &name) {
    for (const auto &v : kept) if (v.toObject().value("name") == name) return v.toObject().value("shown").toString();
    return {};
  }
  void change(const QString &name, const QString &shown) {
    for (auto &&v : kept) {
      QJsonObject o = v.toObject();
      if (o.value("name") == name) { o["shown"] = shown; v = o; }
    }
  }
  void select(int row) {
    view->selectionModel()->select(view->model()->index(row, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  }
  // opens the value cell of `row`, returning its editor
  QLineEdit *edit(int row) {
    const QModelIndex i = view->model()->index(row, 1);
    view->setCurrentIndex(i);
    view->edit(i);
    return view->viewport()->findChild<QLineEdit *>();
  }
};

}  // namespace

class TestKeptPanel : public QObject {
  Q_OBJECT

private slots:
  void twoKeptValuesAppear();
  void typingOverAValueSendsSet();
  void aRefusedSetLeavesTheOldValue();
  void aRefreshWhileEditingLeavesTheEditorAlone();
  void aRefreshKeepsTheSelection();
  void forgetSendsForgetForTheSelection();
  void forgetNeedsASelection();
  void undoSendsUndoAndTheAnswerIsTheLine();
  void nothingKeptShowsTheText();
  void noGameMeansAnEmptyTableAndNoRequest();
  void buttonsAreDisabledWhileWorking();
  void itIsLastInTheWindowsColumn();
  void anEditCommittedWhileBusyIsSaid();
  void aFailedReadIsShownInThePanel();
  void undoNeedsAGame();
};

void TestKeptPanel::twoKeptValuesAppear() {
  KeptRig h;
  QVERIFY(h.start());
  QTRY_COMPARE(h.view->model()->rowCount(), 2);
  QCOMPARE(h.panel->objectName(), QString("keptPanel"));
  QCOMPARE(h.cell(0, 0), QString("points"));
  QCOMPARE(h.cell(0, 1), QString("10"));
  QCOMPARE(h.cell(0, 2), QString("whole number"));
  QCOMPARE(h.cell(1, 0), QString("xp"));
  QCOMPARE(h.cell(1, 1), QString("92"));
  QCOMPARE(h.cell(1, 3), QString("held at 95"));
  QCOMPARE(h.view->model()->headerData(3, Qt::Horizontal).toString(), QString("note"));
  QVERIFY(!h.nothing->isVisible());
  for (const auto &s : h.rig.fake.seen())
    if (s.cmd == "kept") QVERIFY(!s.record);
}

void TestKeptPanel::typingOverAValueSendsSet() {
  KeptRig h;
  h.rig.fake.on("set", [&h](const QStringList &a, const Opts &) {
    h.change(a[0], a[1]);
    return ok(QJsonObject{}, "points is now 5");
  });
  QVERIFY(h.start());
  QTRY_COMPARE(h.view->model()->rowCount(), 2);
  QLineEdit *e = h.edit(0);
  QVERIFY(e);
  e->selectAll();
  QTest::keyClicks(e, "5");
  QTest::keyClick(e, Qt::Key_Return);
  QTRY_COMPARE(timesSeen(h.rig.fake, "set"), 1);
  FakeCore::Seen s;
  for (const auto &x : h.rig.fake.seen()) if (x.cmd == "set") s = x;
  QCOMPARE(s.args, (QStringList{"points", "5"}));
  QVERIFY(s.opts.isEmpty());
  QVERIFY(s.record);
  QTRY_COMPARE(h.cell(0, 1), QString("5"));
  QTRY_COMPARE(h.rig.board.line(), QString("points is now 5"));
}

void TestKeptPanel::aRefusedSetLeavesTheOldValue() {
  KeptRig h;
  h.rig.fake.on("set", [](const QStringList &, const Opts &) { return refused("no such number: abc"); });
  QVERIFY(h.start());
  QTRY_COMPARE(h.view->model()->rowCount(), 2);
  QLineEdit *e = h.edit(0);
  QVERIFY(e);
  e->selectAll();
  QTest::keyClicks(e, "abc");
  QTest::keyClick(e, Qt::Key_Return);
  QTRY_COMPARE(h.rig.board.line(), QString("no such number: abc"));
  QCOMPARE(h.cell(0, 1), QString("10"));
}

void TestKeptPanel::aRefreshWhileEditingLeavesTheEditorAlone() {
  KeptRig h;
  QVERIFY(h.start());
  QTRY_COMPARE(h.view->model()->rowCount(), 2);
  QLineEdit *e = h.edit(0);
  QVERIFY(e);
  QCOMPARE(e->text(), QString("10"));
  e->setText("77");
  h.change("points", "11");
  h.change("xp", "93");
  const int before = timesSeen(h.rig.fake, "kept");
  h.rig.board.tick();
  QTRY_COMPARE(h.cell(1, 1), QString("93"));
  QVERIFY(timesSeen(h.rig.fake, "kept") > before);
  QCOMPARE(h.view->viewport()->findChild<QLineEdit *>(), e);
  QCOMPARE(e->text(), QString("77"));
  QCOMPARE(h.cell(0, 1), QString("10"));  // not replaced under the editor
  // once editing is over, the next refresh shows the game's value
  h.view->closePersistentEditor(h.view->model()->index(0, 1));
  QTest::keyClick(e, Qt::Key_Escape);
  h.rig.board.tick();
  QTRY_COMPARE(h.cell(0, 1), QString("11"));
}

void TestKeptPanel::aRefreshKeepsTheSelection() {
  KeptRig h;
  QVERIFY(h.start());
  QTRY_COMPARE(h.view->model()->rowCount(), 2);
  h.select(1);
  h.change("xp", "100");
  h.rig.board.tick();
  QTRY_COMPARE(h.cell(1, 1), QString("100"));
  QCOMPARE(h.view->selectionModel()->selectedRows().size(), 1);
  QCOMPARE(h.view->selectionModel()->selectedRows().first().row(), 1);
}

void TestKeptPanel::forgetSendsForgetForTheSelection() {
  KeptRig h;
  h.rig.fake.on("forget", [&h](const QStringList &a, const Opts &) {
    QJsonArray rest;
    for (const auto &v : h.kept) if (v.toObject().value("name") != a[0]) rest.append(v);
    h.kept = rest;
    return ok(QJsonObject{}, "forgot xp");
  });
  QVERIFY(h.start());
  QTRY_COMPARE(h.view->model()->rowCount(), 2);
  h.select(1);
  QTRY_VERIFY(h.forget->isEnabled());
  h.forget->click();
  QTRY_COMPARE(timesSeen(h.rig.fake, "forget"), 1);
  for (const auto &x : h.rig.fake.seen())
    if (x.cmd == "forget") { QCOMPARE(x.args, (QStringList{"xp"})); QVERIFY(x.record); }
  QTRY_COMPARE(h.view->model()->rowCount(), 1);
  QCOMPARE(h.cell(0, 0), QString("points"));
}

void TestKeptPanel::forgetNeedsASelection() {
  KeptRig h;
  QVERIFY(h.start());
  QTRY_COMPARE(h.view->model()->rowCount(), 2);
  QVERIFY(!h.forget->isEnabled());
  h.select(0);
  QTRY_VERIFY(h.forget->isEnabled());
}

void TestKeptPanel::undoSendsUndoAndTheAnswerIsTheLine() {
  KeptRig h;
  QString answer = "nothing to undo";
  h.rig.fake.on("undo", [&answer](const QStringList &, const Opts &) { return ok(QJsonObject{}, answer); });
  QVERIFY(h.start());
  QTRY_VERIFY(h.undo->isEnabled());
  QCOMPARE(h.undo->text(), QString("undo last change"));
  h.undo->click();
  QTRY_COMPARE(h.rig.board.line(), QString("nothing to undo"));
  for (const auto &x : h.rig.fake.seen())
    if (x.cmd == "undo") { QVERIFY(x.args.isEmpty()); QVERIFY(x.record); }
  answer = "put points back to 10";
  QTRY_VERIFY(h.undo->isEnabled());
  h.undo->click();
  QTRY_COMPARE(h.rig.board.line(), QString("put points back to 10"));
}

void TestKeptPanel::nothingKeptShowsTheText() {
  KeptRig h;
  h.kept = QJsonArray();
  QVERIFY(h.start());
  QTRY_VERIFY(h.nothing->isVisible());
  QCOMPARE(h.nothing->text(), QString("nothing kept yet. select a result and press keep."));
  QVERIFY(!h.view->isVisible());
  h.kept = QJsonArray{item("xp", "0x6c551068", "whole number", "92")};
  h.rig.board.tick();
  QTRY_VERIFY(h.view->isVisible());
  QVERIFY(!h.nothing->isVisible());
}

void TestKeptPanel::noGameMeansAnEmptyTableAndNoRequest() {
  KeptRig h;
  QVERIFY(h.start());
  QTRY_COMPARE(h.view->model()->rowCount(), 2);
  h.rig.fake.on("status", [](const QStringList &, const Opts &) {
    return ok(QJsonObject{{"attached", QJsonValue()}, {"hunt", QJsonValue()}}, "not attached");
  });
  h.rig.board.tick();  // this tick's own read goes out while the board still shows the game
  QTRY_VERIFY(!h.rig.board.attached().has_value());
  QTRY_COMPARE(h.view->model()->rowCount(), 0);
  QVERIFY(!h.nothing->isVisible());
  QTest::qWait(100);
  const int before = timesSeen(h.rig.fake, "kept");
  h.rig.board.tick();
  QTest::qWait(150);
  QCOMPARE(timesSeen(h.rig.fake, "kept"), before);
  QCOMPARE(h.view->model()->rowCount(), 0);
}

void TestKeptPanel::undoNeedsAGame() {
  KeptRig h;
  QVERIFY(h.start());
  QTRY_VERIFY(h.undo->isEnabled());
  h.rig.fake.on("status", [](const QStringList &, const Opts &) {
    return ok(QJsonObject{{"attached", QJsonValue()}, {"hunt", QJsonValue()}}, "not attached");
  });
  h.rig.board.tick();
  QTRY_VERIFY(!h.rig.board.attached().has_value());
  QTRY_VERIFY(!h.undo->isEnabled());
}

void TestKeptPanel::buttonsAreDisabledWhileWorking() {
  KeptRig h;
  h.rig.fake.hold("undo");
  QVERIFY(h.start());
  QTRY_COMPARE(h.view->model()->rowCount(), 2);
  h.select(0);
  QTRY_VERIFY(h.forget->isEnabled());
  h.undo->click();
  QTRY_VERIFY(h.rig.board.working());
  QVERIFY(!h.undo->isEnabled());
  QVERIFY(!h.forget->isEnabled());
  h.rig.fake.release("undo");
  QTRY_VERIFY(!h.rig.board.working());
  QTRY_VERIFY(h.undo->isEnabled());
}

void TestKeptPanel::itIsLastInTheWindowsColumn() {
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setTerminal(false);
  w.setPolling(false);
  auto *panel = w.findChild<KeptPanel *>("keptPanel");
  QVERIFY(panel);
  QCOMPARE(w.column()->itemAt(w.column()->count() - 1)->widget(), panel);
  QCOMPARE(w.column()->count(), 4);
  QVERIFY(w.findChild<QTableView *>("kept"));
}

// F4: a value typed while moth turns busy is not dropped silently: the line says what is running
void TestKeptPanel::anEditCommittedWhileBusyIsSaid() {
  KeptRig h;
  QVERIFY(h.start());
  QTRY_COMPARE(h.view->model()->rowCount(), 2);
  QLineEdit *e = h.edit(0);
  QVERIFY(e);
  e->selectAll();
  QTest::keyClicks(e, "5");
  h.rig.fake.push({{"busy", QJsonObject{{"actor", "agent"}, {"cmd", "next"}, {"args", QJsonArray{"down"}}}}});
  QTRY_VERIFY(h.rig.board.working());
  QTest::keyClick(e, Qt::Key_Return);
  QTRY_COMPARE(h.rig.board.line(), QString("moth is busy: next down (agent)"));
  QTest::qWait(100);
  QCOMPARE(timesSeen(h.rig.fake, "set"), 0);
  QCOMPARE(h.cell(0, 1), QString("10"));
}

// F7: a `kept` read that fails says so in the panel, in the core's words, until one works; the
// window's line is left to the user's actions
void TestKeptPanel::aFailedReadIsShownInThePanel() {
  KeptRig h;
  QVERIFY(h.start());
  QTRY_COMPARE(h.view->model()->rowCount(), 2);
  const QString line = h.rig.board.line();
  bool failing = true;
  h.rig.fake.on("kept", [&](const QStringList &, const Opts &) {
    return failing ? refused("couldn't read the kept values") : ok(h.kept);
  });
  h.rig.board.tick();
  QTRY_VERIFY(h.nothing->isVisible());
  QCOMPARE(h.nothing->text(), QString("couldn't read the kept values"));
  QVERIFY(h.view->isVisible());  // the values it had stay
  QCOMPARE(h.rig.board.line(), line);

  h.kept = QJsonArray();
  h.rig.board.tick();  // still failing, with nothing kept as far as it knows
  QTest::qWait(100);
  QCOMPARE(h.nothing->text(), QString("couldn't read the kept values"));
  failing = false;
  h.rig.board.tick();
  QTRY_COMPARE(h.nothing->text(), QString("nothing kept yet. select a result and press keep."));
  QVERIFY(h.nothing->isVisible());
  QVERIFY(!h.view->isVisible());
  h.kept = QJsonArray{item("xp", "0x6c551068", "whole number", "92")};
  h.rig.board.tick();
  QTRY_VERIFY(h.view->isVisible());
  QVERIFY(!h.nothing->isVisible());
}

QTEST_MAIN(TestKeptPanel)
#include "test_kept_panel.moc"
