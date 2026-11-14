#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QGuiApplication>
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

// the working folder, put back when the test ends however it ends
class WorkingFolder {
public:
  WorkingFolder() : was_(QDir::currentPath()) {}
  ~WorkingFolder() { QDir::setCurrent(was_); }

private:
  QString was_;
};

// the clipboard's text, put back when the test ends however it ends
class ClipboardText {
public:
  ClipboardText() : was_(QGuiApplication::clipboard()->text()) {}
  ~ClipboardText() { QGuiApplication::clipboard()->setText(was_); }

private:
  QString was_;
};

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
  ~Env() {  // newest first, so a name set twice ends up as it was before the first time
    for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
      if (it->second) qputenv(it->first, *it->second);
      else qunsetenv(it->first);
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

bool stopped(const QString &text) {  // is there a stopped process whose command line has this
  QProcess p;
  p.start("/bin/ps", {"-axo", "stat=,args="});
  p.waitForFinished();
  for (const QString &line : QString::fromUtf8(p.readAllStandardOutput()).split('\n'))
    if (line.trimmed().startsWith('T') && line.contains(text)) return true;
  return false;
}

// a row of the screen that is `text` and nothing else
bool showsRow(const TerminalView *v, const QString &text, int ms = 5000) {
  return QTest::qWaitFor(
      [&] {
        for (int r = 0; r < v->screen()->rows(); ++r)
          if (v->screen()->text(r).trimmed() == text) return true;
        return false;
      },
      ms);
}

// zsh as the user's shell, reading none of the owner's start-up files: HOME and ZDOTDIR are the
// test's own folder, whose .zshrc sets a prompt that ends in $
void useZsh(Shell &shell) {
  shell.env.set("SHELL", "/bin/zsh");
  shell.env.set("ZDOTDIR", shell.home.path().toUtf8());
  QFile rc(shell.home.path() + "/.zshrc");
  if (rc.open(QIODevice::WriteOnly)) rc.write("PS1='$ '\n");
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
  void anEnvPutsBackWhatWasSetTwice();
  void theTerminalPathsAndEnvironment();
  void theCommandAfterItCannotEatTheShellThatFollows();
  void aCommandThatStopsItselfIsResumedUnderShAndZsh();
  void hidingAndShowingTheWindowDoesNotStartTheTerminalAgain();
  void aTitleWithLineSeparatorsKeepsTheStripOneLine();
  void whenTheShellEndedCommandCCopiesTheLastScreen();
  void aRefusedOrFailedQuietLeavesTheBoxAndSaysWhy();
  void theQuietBoxIsOutOfReachWhileStale();
  void theTitleIsDrawnCutShort();
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

void TestMainWindow::anEnvPutsBackWhatWasSetTwice() {
  qunsetenv("MOTH_TEST_TWICE");
  qputenv("MOTH_TEST_SET", "original");
  {
    Env env;
    env.set("MOTH_TEST_TWICE", "one");
    env.set("MOTH_TEST_TWICE", "two");
    env.unset("MOTH_TEST_SET");
    env.set("MOTH_TEST_SET", "x");
    env.unset("MOTH_TEST_SET");
    QVERIFY(!qEnvironmentVariableIsSet("MOTH_TEST_SET"));
  }
  QVERIFY(!qEnvironmentVariableIsSet("MOTH_TEST_TWICE"));
  QCOMPARE(qgetenv("MOTH_TEST_SET"), QByteArray("original"));
  qunsetenv("MOTH_TEST_SET");
}

void TestMainWindow::theTerminalPathsAndEnvironment() {
  {
    Env env;
    env.unset("MOTH_TERMINAL");
    QCOMPARE(terminalCommand("/opt/my moth/moth"), QString("'/opt/my moth/moth' agent"));
    QCOMPARE(terminalCommand("/opt/it's/moth"), QString("'/opt/it'\\''s/moth' agent"));
    env.set("MOTH_TERMINAL", "echo hi");
    QCOMPARE(terminalCommand("/opt/moth"), QString("echo hi"));
    env.set("MOTH_TERMINAL", "");
    QCOMPARE(terminalCommand("/opt/moth"), QString());
    env.unset("SHELL");
    QCOMPARE(userShell(), QString("/bin/zsh"));
    env.set("SHELL", "/bin/sh");
    QCOMPARE(userShell(), QString("/bin/sh"));
  }
  Env env;
  env.set("SHELL", "/bin/sh");
  env.set("MOTH_ACTOR", "agent");
  env.set("PATH", "/usr/bin:/bin");
  for (const char *name : {"TERM_PROGRAM_VERSION", "TERM_SESSION_ID", "TMUX", "TMUX_PANE", "ITERM_SESSION_ID", "CLAUDECODE",
                           "CLAUDE_CODE_ENTRYPOINT"})
    env.set(name, "from the launcher");
  env.set("TERM_PROGRAM", "Apple_Terminal");
  env.set("CLAUDE_CONFIG_DIR", "/keep/me");
  env.set("ANTHROPIC_API_KEY", "keep");
  const QStringList vars = terminalEnv("/tmp/a home", "/opt/my moth/moth");
  QVERIFY(vars.contains("MOTH_HOME=/tmp/a home"));
  QVERIFY(vars.contains("MOTH_BIN=/opt/my moth/moth"));
  QVERIFY(vars.contains("PATH=/opt/my moth:/usr/bin:/bin"));
  QVERIFY(vars.contains("TERM=xterm-256color"));
  QVERIFY(vars.contains("COLORTERM=truecolor"));
  QVERIFY(vars.contains("SHELL=/bin/sh"));
  for (const QString &v : vars) QVERIFY2(!v.startsWith("MOTH_ACTOR"), qPrintable(v));
  // what belongs to the launching terminal or session is not passed on; the agent's sign-in is
  for (const char *name : {"TERM_PROGRAM_VERSION", "TERM_SESSION_ID", "TMUX", "TMUX_PANE", "ITERM_SESSION_ID", "CLAUDECODE",
                           "CLAUDE_CODE_ENTRYPOINT"})
    for (const QString &v : vars) QVERIFY2(!v.startsWith(QString(name) + "="), qPrintable(v));
  QVERIFY(vars.contains("TERM_PROGRAM=moth"));
  QVERIFY(vars.contains("CLAUDE_CONFIG_DIR=/keep/me"));
  QVERIFY(vars.contains("ANTHROPIC_API_KEY=keep"));
  // MOTH_BIN is passed on as an absolute path
  WorkingFolder folder;
  QDir::setCurrent("/tmp");
  QVERIFY(terminalEnv("/tmp/h", "rel/moth").contains("MOTH_BIN=" + QDir::currentPath() + "/rel/moth"));
}

// R20: whatever the command ends with, a shell follows, and nothing is said of jobs when nothing
// stopped (a command that ends above 128 makes fg look once, quietly)
void TestMainWindow::theCommandAfterItCannotEatTheShellThatFollows() {
  for (const QString &command : {QString("echo one &"), QString("echo two;"), QString("echo three # a comment"),
                                 QString("/bin/sh -c 'exit 200'")}) {
    Shell shell(command);
    Rig rig;
    QVERIFY(rig.ready());
    MainWindow w(&rig.board);
    w.setPolling(false);
    showOffscreen(w);
    TerminalView *t = w.terminal();
    QVERIFY(t);
    QVERIFY2(prompted(t), qPrintable(command + "\n" + screenText(*t)));
    QVERIFY2(!screenText(*t).contains("syntax error"), qPrintable(screenText(*t)));
    QVERIFY2(!screenText(*t).contains("job") && !screenText(*t).contains("fg"), qPrintable(screenText(*t)));
    QVERIFY(t->running());
  }
}

// I5: Control+Z in the agent bounces back. A command that stops itself is resumed by itself (nothing
// typed), the shell follows once it ends, and nothing stopped is left; under sh and under zsh.
// Under zsh, a command that ends above 128 without stopping says nothing of jobs either
void TestMainWindow::aCommandThatStopsItselfIsResumedUnderShAndZsh() {
  for (const bool zsh : {false, true}) {
    const QString which = zsh ? "zsh" : "sh";
    QTemporaryDir bin("/tmp/moth-bin-XXXXXX");
    const QString standIn = bin.path() + "/stops";
    QFile f(standIn);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("#!/bin/sh\necho up\nkill -TSTP $$\necho back\nread x\necho \"got $x\"\n");
    f.close();
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    {
      Shell shell("'" + standIn + "'");
      if (zsh) useZsh(shell);
      Rig rig;
      QVERIFY(rig.ready());
      MainWindow w(&rig.board);
      w.setPolling(false);
      showOffscreen(w);
      TerminalView *t = w.terminal();
      QVERIFY(t);
      QVERIFY2(showsRow(t, "up"), qPrintable(which + "\n" + screenText(*t)));
      QVERIFY2(showsRow(t, "back"), qPrintable(which + "\n" + screenText(*t)));  // nothing typed
      QVERIFY2(!stopped(standIn), qPrintable(which));
      type(t, "done");
      QVERIFY2(showsRow(t, "got done"), qPrintable(which + "\n" + screenText(*t)));
      QVERIFY2(prompted(t), qPrintable(which + "\n" + screenText(*t)));
      QVERIFY2(QTest::qWaitFor([&] { return !processes(standIn); }, 2000), qPrintable(which));  // not stopped, not there at all
      QVERIFY(t->running());
    }
    if (zsh) {
      Shell shell("/bin/sh -c 'exit 200'");
      useZsh(shell);
      Rig rig;
      QVERIFY(rig.ready());
      MainWindow w(&rig.board);
      w.setPolling(false);
      showOffscreen(w);
      TerminalView *t = w.terminal();
      QVERIFY2(prompted(t), qPrintable(screenText(*t)));
      QVERIFY2(!screenText(*t).contains("job") && !screenText(*t).contains("fg"), qPrintable(screenText(*t)));
    }
  }
}

void TestMainWindow::hidingAndShowingTheWindowDoesNotStartTheTerminalAgain() {
  Shell shell("");
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  showOffscreen(w);
  TerminalView *t = w.terminal();
  QVERIFY(prompted(t));
  const qint64 pid = t->pid();
  w.hide();
  w.show();
  QTest::qWait(300);
  QCOMPARE(w.terminal(), t);
  QCOMPARE(t->pid(), pid);
  QVERIFY(t->running());
}

void TestMainWindow::aTitleWithLineSeparatorsKeepsTheStripOneLine() {
  Shell shell("");
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  showOffscreen(w);
  auto *title = w.findChild<QLabel *>("terminalTitle");
  QVERIFY(prompted(w.terminal()));
  w.terminal()->screen()->feed("\x1b]2;one\x07");
  const int one = title->height(), rows = w.terminal()->screen()->rows();
  w.terminal()->screen()->feed("\x1b]2;two\xe2\x80\xa8" "three\xe2\x80\xa9" "four\x07");
  QCoreApplication::processEvents();
  QCOMPARE(title->text(), QString("two three four"));
  QCOMPARE(title->height(), one);
  QCOMPARE(w.terminal()->screen()->rows(), rows);
}

void TestMainWindow::whenTheShellEndedCommandCCopiesTheLastScreen() {
  Shell shell("");
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  showOffscreen(w);
  TerminalView *t = w.terminal();
  QVERIFY(prompted(t));
  type(t, "echo marker");
  QVERIFY(shows(t, "marker"));
  type(t, "exit");
  QTRY_VERIFY(!t->running());
  int row = -1;
  for (int r = 0; r < t->screen()->rows() && row < 0; ++r)
    if (t->screen()->text(r).startsWith("marker")) row = r;
  QVERIFY(row >= 0);
  ClipboardText clipboard;
  QGuiApplication::clipboard()->setText("untouched");
  const int cellH = t->viewport()->height() / t->screen()->rows();
  QTest::mouseDClick(t->viewport(), Qt::LeftButton, {}, QPoint(3, row * cellH + cellH / 2));
  QCOMPARE(t->selectedText(), QString("marker"));
  QTest::keyClick(t, Qt::Key_C, Qt::ControlModifier);  // Command, as Qt reports it on macOS
  QCOMPARE(QGuiApplication::clipboard()->text(), QString("marker"));
  QVERIFY(!t->running());  // nothing else started
}

// a refused `quiet on` (the board is working) and a failed one leave the box as it was and say why
void TestMainWindow::aRefusedOrFailedQuietLeavesTheBoxAndSaysWhy() {
  Shell shell("");
  Rig rig;
  rig.fake.statusAsWritten();
  rig.fake.on("status", [](const QStringList &, const Opts &) {
    return Reply{true, QJsonObject{{"attached", QJsonValue::Null}, {"hunt", QJsonValue::Null}, {"proto", 2}, {"quiet", false}},
                 "not attached", ""};
  });
  rig.fake.on("quiet", [](const QStringList &, const Opts &) { return Reply{false, QJsonValue(), "", "quiet is broken"}; });
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  showOffscreen(w);
  auto *box = w.findChild<QCheckBox *>("quiet");
  auto *line = w.findChild<QLabel *>("line");
  QTRY_VERIFY(box->isEnabled());
  QVERIFY(!box->isChecked());
  // answered with an error
  QTest::mouseClick(box, Qt::LeftButton, {}, box->rect().center());
  QTRY_COMPARE(timesSeen(rig.fake, "quiet"), 1);
  QTRY_COMPARE(line->text(), QString("quiet is broken"));
  QVERIFY(!box->isChecked());
  // refused: one of the user's own requests is unanswered, and the box sends nothing
  rig.fake.on("find", [](const QStringList &, const Opts &) { return Reply{true, QJsonObject{{"main", 5}}, "5 matches", ""}; });
  rig.fake.hold("find");
  QVERIFY(rig.board.run("find", {"100"}));
  QTRY_COMPARE(timesSeen(rig.fake, "find"), 1);
  QTRY_VERIFY(!box->isEnabled());  // out of reach, so forced within it: a click that gets through is refused
  box->setEnabled(true);
  QTest::mouseClick(box, Qt::LeftButton, {}, box->rect().center());
  QCOMPARE(timesSeen(rig.fake, "quiet"), 1);
  // the board says it is busy; the window's line, while something works, shows what works
  QCOMPARE(rig.board.line(), QString("moth is busy: find 100"));
  QCOMPARE(line->text(), QString::fromUtf8("working: find 100 (user)…"));
  QVERIFY(!box->isChecked());
  rig.fake.release("find");
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
  Shell shell("");
  shell.env.unset("MOTH_TERMINAL");  // the default command: moth by its full path, then `agent`
  shell.env.set("MOTH_CLAUDE", "/usr/bin/false");  // a second guard: no real agent whatever runs
  QTemporaryDir bin("/tmp/moth-bin-XXXXXX");
  QFile stub(bin.path() + "/moth");
  QVERIFY(stub.open(QIODevice::WriteOnly));
  const QString marker = bin.path() + "/ran";
  stub.write("#!/bin/sh\nif [ \"$1\" = agent ]; then : > '" + marker.toUtf8() + "'; echo '" + sentence.toUtf8() +
             "'; exit 1; fi\n");
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
  QVERIFY2(QFile::exists(marker), "the stand-in moth was not the one run");

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

void TestMainWindow::theQuietBoxIsOutOfReachWhileStale() {
  Shell shell("");
  Rig rig;
  rig.fake.statusAsWritten();
  rig.fake.on("status", [](const QStringList &, const Opts &) {
    return Reply{true, QJsonObject{{"attached", QJsonValue::Null}, {"hunt", QJsonValue::Null}, {"proto", 1}}, "not attached", ""};
  });
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  showOffscreen(w);
  auto *box = w.findChild<QCheckBox *>("quiet");
  rig.board.tick();
  QTRY_VERIFY(rig.board.stale());
  QTRY_VERIFY(!box->isEnabled());
  QTest::mouseClick(box, Qt::LeftButton, {}, box->rect().center());
  QCOMPARE(timesSeen(rig.fake, "quiet"), 0);
}

// what is painted is the title cut short with an ellipsis, not the whole title run off the edge
void TestMainWindow::theTitleIsDrawnCutShort() {
  Shell shell("");
  Rig rig;
  QVERIFY(rig.ready());
  MainWindow w(&rig.board);
  w.setPolling(false);
  showOffscreen(w);
  auto *title = w.findChild<QLabel *>("terminalTitle");
  QVERIFY(prompted(w.terminal()));
  w.terminal()->screen()->feed("\x1b]2;" + QByteArray(2000, 'x') + "\x07");
  QCoreApplication::processEvents();
  QImage img(title->size(), QImage::Format_ARGB32);
  img.fill(Qt::transparent);
  title->render(&img);
  // the right edge is blank where a whole title would still be drawn, and the ellipsis is inked
  const QString shown = title->fontMetrics().elidedText(title->text(), Qt::ElideRight, title->contentsRect().width());
  QVERIFY(shown.endsWith(QString::fromUtf8("…")));
  QVERIFY(shown.size() < title->text().size());
  QVERIFY(title->fontMetrics().horizontalAdvance(title->text()) > title->width() * 4);
  // no ink in the last pixel column beyond what the elided text takes: the ellipsis ends inside the label
  const QRgb ground = img.pixel(0, 0);
  bool inkAtEdge = false;
  for (int y = 0; y < img.height(); ++y)
    if (img.pixel(img.width() - 1, y) != ground) inkAtEdge = true;
  QVERIFY(!inkAtEdge);
  bool inked = false;
  for (int y = 0; y < img.height() && !inked; ++y)
    for (int x = img.width() / 2; x < img.width(); ++x)
      if (img.pixel(x, y) != ground) { inked = true; break; }
  QVERIFY(inked);
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
