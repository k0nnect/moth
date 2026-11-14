#include <QCheckBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QSplitter>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtTest>
#include <cerrno>
#include <csignal>

#include "fake_core.hpp"
#include "main_window.hpp"
#include "paths.hpp"
#include "term_screen.hpp"
#include "terminal_view.hpp"

namespace {

// the offscreen platform warns that it can't pass size hints on, each time a window is shown
void showOffscreen(QWidget &w) {
  QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
  w.show();
}

// environment variables set for a test and put back as they were
class Env {
public:
  void set(const char *name, const QByteArray &value) {
    save(name);
    qputenv(name, value);
  }
  void unset(const char *name) {
    save(name);
    qunsetenv(name);
  }
  ~Env() {
    for (const auto &[name, value] : saved_) {
      if (value) qputenv(name, *value);
      else qunsetenv(name);
    }
  }

private:
  void save(const char *name) {
    saved_.append({name, qEnvironmentVariableIsSet(name) ? std::optional<QByteArray>(qgetenv(name)) : std::nullopt});
  }
  QList<std::pair<const char *, std::optional<QByteArray>>> saved_;
};

// a shell that reads none of the owner's start-up files, and the command it runs first
struct Shell {
  QTemporaryDir home{"/tmp/moth-home-XXXXXX"};
  Env env;
  explicit Shell(const QString &command) {
    env.set("SHELL", "/bin/sh");
    env.set("HOME", home.path().toUtf8());
    env.set("MOTH_TERMINAL", command.toUtf8());
    env.unset("MOTH_BIN");
  }
};

QString screenText(const TerminalView &v) {
  QStringList rows;
  for (int r = 0; r < v.screen()->rows(); ++r) rows << v.screen()->text(r);
  return rows.join('\n');
}

bool shows(const TerminalView *v, const QString &text, int ms = 5000) {
  return QTest::qWaitFor([&] { return screenText(*v).contains(text); }, ms);
}

// sh's prompt, on its row of its own, with nothing typed after it
bool prompted(const TerminalView *v, int ms = 5000) {
  return QTest::qWaitFor(
      [&] {
        for (int r = 0; r < v->screen()->rows(); ++r)
          if (v->screen()->text(r).trimmed().endsWith("$")) return true;
        return false;
      },
      ms);
}

void type(TerminalView *v, const QString &line) {
  QTest::keyClicks(v, line);
  QTest::keyClick(v, Qt::Key_Return);
}

bool processes(const QString &pattern) {  // is there a process whose command line has this
  QProcess p;
  p.start("/usr/bin/pgrep", {"-f", pattern});
  p.waitForFinished();
  return p.exitCode() == 0;
}

}  // namespace

class TestMainWindow : public QObject {
  Q_OBJECT

private slots:
  void itsShapeAndItsLine();
  void theLineGrowsToShowAWholeAnswer();
  void pollsWhileShown();
  void pollingCanBeKeptOff();
  void theStaleSentenceStaysWhileSomeoneWorks();
  void theTerminalPathsAndEnvironment();
  void theWindowOpensWithTheColumnAndTheTerminal();
  void theTerminalRunsItsCommandThenAShellWithMothHome();
  void aMissingAgentLeavesAShellAndThePanelsWork();
  void theStripShowsTheTitleAndCutsALongOne();
  void theQuietBoxShowsTheBoardAndAsksIt();
  void theQuietBoxIsOutOfReachWhileBusy();
  void closingTheWindowHangsUpOnWhatRuns();
  void whenTheShellEndsReturnStartsItAgain();
  void withTheTerminalOffNothingIsStarted();
};

void TestMainWindow::itsShapeAndItsLine() {
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setTerminal(false);
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
  // the window was made wide for a terminal and then narrowed: the platform is told its least size again
  QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
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

// R13: the user's own action shows the core's whole answer, which can be several lines
// R10: a service from another build is said even while a request runs on it
void TestMainWindow::theStaleSentenceStaysWhileSomeoneWorks() {
  Rig rig;
  QVERIFY(rig.ready());
  rig.fake.statusAsWritten();
  rig.fake.on("status", [](const QStringList &, const Opts &) {
    return Reply{true, QJsonObject{{"attached", QJsonValue::Null}, {"hunt", QJsonValue::Null}, {"proto", 1}}, "not attached", ""};
  });
  MainWindow w(&rig.board);
  w.setTerminal(false);
  w.setPolling(false);
  auto *line = w.findChild<QLabel *>("line");
  rig.board.tick();
  const QString stale = "moth's background service is from another build of moth. stop it and it starts again by itself: moth stop";
  QTRY_COMPARE(line->text(), stale);
  rig.fake.push({{"busy", QJsonObject{{"actor", "agent"}, {"cmd", "find"}, {"args", QJsonArray{"100"}}}}});
  QTRY_VERIFY(rig.board.working());
  QTest::qWait(200);
  QCOMPARE(line->text(), stale);
}

void TestMainWindow::theLineGrowsToShowAWholeAnswer() {
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setTerminal(false);
  w.setPolling(false);
  auto *line = w.findChild<QLabel *>("line");
  QVERIFY(line);
  QVERIFY(line->wordWrap());
  w.resize(560, 420);  // short: the table gives way, not the line
  showOffscreen(w);
  QVERIFY(QTest::qWaitForWindowExposed(&w));
  const int one = line->fontMetrics().lineSpacing();
  // the longer line raises the window's least height, which the platform warns it can't pass on
  QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
  rig.board.say("set #3 to 9\nthe game put it straight back to 100. it's worked out from something else, so writing here "
                "won't stick, whatever is typed over it and however often.\na third line");
  QTRY_VERIFY2(line->height() >= 4 * one, qPrintable(QString("%1 high, %2 a line").arg(line->height()).arg(one)));
  QVERIFY(line->mapTo(&w, QPoint(0, line->height())).y() <= w.centralWidget()->height());
}

void TestMainWindow::pollsWhileShown() {
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setTerminal(false);
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
  w.setTerminal(false);
  w.setPolling(false);
  const int before = timesSeen(rig.fake, "status");
  showOffscreen(w);
  QTest::qWait(1500);
  QCOMPARE(timesSeen(rig.fake, "status"), before);
}

void TestMainWindow::theTerminalPathsAndEnvironment() {
  {
    Env env;
    env.unset("MOTH_TERMINAL");
    QCOMPARE(terminalCommand(), QString("moth agent"));
    env.set("MOTH_TERMINAL", "echo hi");
    QCOMPARE(terminalCommand(), QString("echo hi"));
    env.set("MOTH_TERMINAL", "");
    QCOMPARE(terminalCommand(), QString());
    env.unset("SHELL");
    QCOMPARE(userShell(), QString("/bin/zsh"));
    env.set("SHELL", "/bin/sh");
    QCOMPARE(userShell(), QString("/bin/sh"));
  }
  Env env;
  env.set("SHELL", "/bin/sh");
  env.set("MOTH_ACTOR", "agent");
  env.set("PATH", "/usr/bin:/bin");
  const QStringList vars = terminalEnv("/tmp/a home", "/opt/my moth/moth");
  QVERIFY(vars.contains("MOTH_HOME=/tmp/a home"));
  QVERIFY(vars.contains("MOTH_BIN=/opt/my moth/moth"));
  QVERIFY(vars.contains("PATH=/opt/my moth:/usr/bin:/bin"));
  QVERIFY(vars.contains("TERM=xterm-256color"));
  QVERIFY(vars.contains("COLORTERM=truecolor"));
  QVERIFY(vars.contains("SHELL=/bin/sh"));
  for (const QString &v : vars) QVERIFY2(!v.startsWith("MOTH_ACTOR"), qPrintable(v));
}

void TestMainWindow::theWindowOpensWithTheColumnAndTheTerminal() {
  Shell shell("");
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  showOffscreen(w);
  QVERIFY(QTest::qWaitForWindowExposed(&w));
  QCOMPARE(w.size(), QSize(1280, 860));
  auto *split = qobject_cast<QSplitter *>(w.centralWidget());
  QVERIFY(split);
  QCOMPARE(split->count(), 2);
  QCOMPARE(split->sizes().value(0), 520);
  TerminalView *t = w.terminal();
  QVERIFY(t);
  QCOMPARE(t->objectName(), QString("terminal"));
  QCOMPARE(w.findChild<QLabel *>("terminalTitle")->text(), QString("terminal"));
  QVERIFY(w.findChild<QCheckBox *>("quiet"));
  QVERIFY(t->screen()->rows() > 24);  // it has the size its place gives it, not the first 24 by 80
  QVERIFY(prompted(t));
  QVERIFY(t->hasFocus());
  // the terminal gives way first; the column never goes under 420, whatever the splitter is asked
  w.resize(700, 860);
  QTRY_COMPARE(split->sizes().value(0), 520);
  QVERIFY(split->sizes().value(1) < 200);
  split->setSizes({100, 600});
  QCOMPARE(split->sizes().value(0), 420);
  QCOMPARE(split->sizes().value(0) + split->sizes().value(1) + split->handleWidth(), split->width());
}

void TestMainWindow::theTerminalRunsItsCommandThenAShellWithMothHome() {
  Shell shell("printf \"from the terminal\\n\"");
  Rig rig("/tmp/moth home-XXXXXX");  // a space in it
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  showOffscreen(w);
  TerminalView *t = w.terminal();
  QVERIFY(t);
  QVERIFY2(shows(t, "from the terminal"), qPrintable(screenText(*t)));
  QVERIFY(prompted(t));
  type(t, "echo $MOTH_HOME; pwd");
  QVERIFY2(shows(t, rig.home.path()), qPrintable(screenText(*t)));
  QVERIFY2(shows(t, shell.home.path()), qPrintable(screenText(*t)));  // in the home folder
}

void TestMainWindow::aMissingAgentLeavesAShellAndThePanelsWork() {
  const QString sentence =
      "claude code is not installed, or not on PATH. moth works without it: use the window, or type moth commands here.";
  Shell shell("moth agent");
  QTemporaryDir bin("/tmp/moth-bin-XXXXXX");
  QFile stub(bin.path() + "/moth");
  QVERIFY(stub.open(QIODevice::WriteOnly));
  stub.write("#!/bin/sh\nif [ \"$1\" = agent ]; then echo '" + sentence.toUtf8() + "'; exit 1; fi\n");
  stub.close();
  stub.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
  shell.env.set("MOTH_BIN", (bin.path() + "/moth").toUtf8());

  Rig rig;
  rig.fake.statusAsWritten();
  rig.fake.on("status", [](const QStringList &, const Opts &) {
    return Reply{true, QJsonObject{{"attached", QJsonObject{{"pid", 4242}, {"name", "disco.exe"}, {"bits", 64}, {"bottle", "uhmm"}}},
                                   {"hunt", QJsonValue::Null}, {"proto", 2}, {"quiet", false}},
                 "attached to disco.exe", ""};
  });
  rig.fake.on("find", [](const QStringList &, const Opts &) { return Reply{true, QJsonObject{{"main", 5}}, "5 matches", ""}; });
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  // the game bar's words set a least size, which the platform is told of, as the window is shown
  for (int i = 0; i < 2; ++i) QTest::ignoreMessage(QtWarningMsg, "This plugin does not support propagateSizeHints()");
  showOffscreen(w);
  rig.board.tick();
  QTRY_VERIFY(rig.board.attached().has_value());
  TerminalView *t = w.terminal();
  QVERIFY(t);
  // the sentence wraps over rows: compare without the spaces a wrap may swallow
  auto flat = [](QString x) { return x.remove(' ').remove('\n'); };
  QVERIFY2(QTest::qWaitFor([&] { return flat(screenText(*t)).contains(flat(sentence)); }, 5000), qPrintable(screenText(*t)));
  QVERIFY(prompted(t));
  QVERIFY(t->running());

  auto *number = w.findChild<QLineEdit *>("number");
  QVERIFY(number);
  QTest::keyClicks(number, "100");
  QTest::keyClick(number, Qt::Key_Return);
  QTRY_COMPARE(timesSeen(rig.fake, "find"), 1);
}

void TestMainWindow::theStripShowsTheTitleAndCutsALongOne() {
  Shell shell("");
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  showOffscreen(w);
  auto *title = w.findChild<QLabel *>("terminalTitle");
  QVERIFY(title);
  QVERIFY(prompted(w.terminal()));
  w.terminal()->screen()->feed("\x1b]2;<b>in the shell</b>\x07");
  QCOMPARE(title->text(), QString("<b>in the shell</b>"));  // plain text
  QCOMPARE(title->textFormat(), Qt::PlainText);
  w.terminal()->screen()->feed("\x1b]2;" + QByteArray(2000, 'x') + "\x07");
  QVERIFY(title->text().size() > 100);
  QVERIFY(title->width() < title->text().size() * 4);  // the label is not as wide as its text
  QVERIFY(w.minimumSizeHint().width() < 1000);
  w.terminal()->screen()->feed("\x1b]2;\x07");
  QCOMPARE(title->text(), QString("terminal"));
}

void TestMainWindow::theQuietBoxShowsTheBoardAndAsksIt() {
  Shell shell("");
  Rig rig;
  rig.fake.statusAsWritten();
  bool quiet = true;
  rig.fake.on("status", [&](const QStringList &, const Opts &) {
    return Reply{true, QJsonObject{{"attached", QJsonValue::Null}, {"hunt", QJsonValue::Null}, {"proto", 2}, {"quiet", quiet}},
                 "not attached", ""};
  });
  rig.fake.on("quiet", [&](const QStringList &args, const Opts &) {
    if (args == QStringList{"on"}) quiet = true;
    else if (args == QStringList{"off"}) quiet = false;
    return Reply{true, QJsonValue(), "ok", ""};
  });
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  showOffscreen(w);
  auto *box = w.findChild<QCheckBox *>("quiet");
  QVERIFY(box);
  QVERIFY(QTest::qWaitForWindowExposed(&w));
  QTRY_VERIFY(box->isChecked());
  QVERIFY(box->isEnabled());

  rig.fake.hold("quiet");
  QTest::mouseClick(box, Qt::LeftButton, {}, box->rect().center());
  QTRY_COMPARE(timesSeen(rig.fake, "quiet"), 1);
  QCOMPARE(rig.fake.seen().last().args, QStringList{"off"});
  QVERIFY(rig.fake.seen().last().record);
  QVERIFY(box->isChecked());  // not ahead of the board
  rig.fake.release("quiet");
  QTRY_VERIFY(!box->isChecked());
  QTRY_VERIFY(box->isEnabled());

  rig.fake.on("quiet", [](const QStringList &, const Opts &) { return Reply{false, QJsonValue(), "", "no"}; });
  QTest::mouseClick(box, Qt::LeftButton, {}, box->rect().center());
  QTRY_COMPARE(timesSeen(rig.fake, "quiet"), 2);
  QTRY_VERIFY(box->isEnabled());
  QVERIFY(!box->isChecked());  // the board still says off: a refused run puts it back
  quiet = true;
  rig.board.tick();
  QTRY_VERIFY(box->isChecked());  // someone else's change
}

void TestMainWindow::theQuietBoxIsOutOfReachWhileBusy() {
  Shell shell("");
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  showOffscreen(w);
  auto *box = w.findChild<QCheckBox *>("quiet");
  QTRY_VERIFY(box->isEnabled());
  rig.fake.push({{"busy", QJsonObject{{"actor", "agent"}, {"cmd", "find"}, {"args", QJsonArray{"100"}}}}});
  QTRY_VERIFY(rig.board.working());
  QTRY_VERIFY(!box->isEnabled());
  QTest::mouseClick(box, Qt::LeftButton, {}, box->rect().center());
  QCOMPARE(timesSeen(rig.fake, "quiet"), 0);
  rig.fake.push({{"event", QJsonObject{{"seq", 1}, {"actor", "agent"}, {"cmd", "find"}, {"args", QJsonArray{"100"}},
                                       {"ok", true}, {"summary", "done"}}}});
  QTRY_VERIFY(box->isEnabled());
}

void TestMainWindow::closingTheWindowHangsUpOnWhatRuns() {
  Shell shell("");
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  showOffscreen(w);
  TerminalView *t = w.terminal();
  QVERIFY(prompted(t));
  const pid_t shellPid = static_cast<pid_t>(t->pid());
  QVERIFY(shellPid > 0);
  type(t, "sleep 298");
  QTRY_VERIFY(processes("sleep 298"));
  w.close();
  QVERIFY(!w.terminal());
  QTRY_VERIFY_WITH_TIMEOUT(!processes("sleep 298"), 2000);
  QVERIFY(::kill(shellPid, 0) == -1 && errno == ESRCH);
}

void TestMainWindow::whenTheShellEndsReturnStartsItAgain() {
  Shell shell("");
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  showOffscreen(w);
  TerminalView *t = w.terminal();
  auto *title = w.findChild<QLabel *>("terminalTitle");
  QVERIFY(prompted(t));
  type(t, "echo before");
  QVERIFY(shows(t, "before"));
  type(t, "exit");
  QTRY_COMPARE(title->text(), QString("the terminal ended. press return in it to start it again."));
  QVERIFY(!t->running());
  QVERIFY(screenText(*t).contains("before"));  // its last screen stays
  QTest::keyClicks(t, "x");
  QTest::keyClick(t, Qt::Key_Escape);
  QTest::qWait(200);
  QVERIFY(!t->running());
  QTest::keyClick(t, Qt::Key_Return);
  QVERIFY(t->running());
  QCOMPARE(title->text(), QString("terminal"));
  QVERIFY(prompted(t));
  type(t, "echo again");
  QVERIFY(shows(t, "again"));
}

void TestMainWindow::withTheTerminalOffNothingIsStarted() {
  Shell shell("sleep 297");
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setTerminal(false);
  w.setPolling(false);
  showOffscreen(w);
  QVERIFY(QTest::qWaitForWindowExposed(&w));
  QTest::qWait(500);
  QVERIFY(!w.terminal());
  QVERIFY(!w.findChild<TerminalView *>());
  QVERIFY(!w.findChild<QCheckBox *>("quiet"));
  QVERIFY(!processes("sleep 297"));
}

QTEST_MAIN(TestMainWindow)
#include "test_main_window.moc"
