#include <QJsonArray>
#include <QLabel>
#include <QSplitter>
#include <QVBoxLayout>
#include <QtTest>

#include "fake_core.hpp"
#include "main_window.hpp"

namespace {

// the offscreen platform warns that it can't pass size hints on, each time a window is shown
void showOffscreen(QWidget &w) {
  QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
  w.show();
}

}  // namespace

class TestMainWindow : public QObject {
  Q_OBJECT

private slots:
  void itsShapeAndItsLine();
  void pollsWhileShown();
  void pollingCanBeKeptOff();
};

void TestMainWindow::itsShapeAndItsLine() {
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  QCOMPARE(w.windowTitle(), QString("moth"));
  QCOMPARE(w.size(), QSize(560, 860));
  auto *split = qobject_cast<QSplitter *>(w.centralWidget());
  QVERIFY(split);
  QCOMPARE(split->count(), 1);

  auto *line = w.findChild<QLabel *>("line");
  QVERIFY(line);
  QCOMPARE(line->text(), QString("not attached"));

  // a panel put in the column sits above the line
  auto *panel = new QLabel("a panel");
  w.column()->addWidget(panel);
  showOffscreen(w);
  QVERIFY(QTest::qWaitForWindowExposed(&w));
  QVERIFY(panel->mapTo(&w, QPoint(0, 0)).y() < line->mapTo(&w, QPoint(0, 0)).y());

  rig.fake.push({{"busy", QJsonObject{{"actor", "agent"}, {"cmd", "find"}, {"args", QJsonArray{"100"}}}}});
  QTRY_COMPARE(line->text(), QString::fromUtf8("working: find 100 (agent)…"));
  rig.fake.push({{"event", QJsonObject{{"seq", 1}, {"actor", "agent"}, {"cmd", "find"}, {"args", QJsonArray{"100"}},
                                       {"ok", true}, {"summary", "241,178 matches"}}}});
  QTRY_COMPARE(line->text(), QString("agent: 241,178 matches"));

  // the core's words as they are, never read as markup
  rig.board.say("not attached. run: moth attach --bottle <bottle> <game.exe>");
  QCOMPARE(line->text(), QString("not attached. run: moth attach --bottle <bottle> <game.exe>"));
  QCOMPARE(line->textFormat(), Qt::PlainText);
}

void TestMainWindow::pollsWhileShown() {
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  const int before = timesSeen(rig.fake, "status");
  showOffscreen(w);
  QTRY_VERIFY_WITH_TIMEOUT(timesSeen(rig.fake, "status") >= before + 2, 3000);  // on showing, then each second
  for (const auto &s : rig.fake.seen()) QCOMPARE(s.record, false);

  w.hide();
  QTest::qWait(100);
  const int hidden = timesSeen(rig.fake, "status");
  QTest::qWait(1500);
  QCOMPARE(timesSeen(rig.fake, "status"), hidden);
}

// what tests that count requests use
void TestMainWindow::pollingCanBeKeptOff() {
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  const int before = timesSeen(rig.fake, "status");
  showOffscreen(w);
  QTest::qWait(1500);
  QCOMPARE(timesSeen(rig.fake, "status"), before);
}

QTEST_MAIN(TestMainWindow)
#include "test_main_window.moc"
