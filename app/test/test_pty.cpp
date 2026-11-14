#include <QtTest>
#include <csignal>
#include <unistd.h>

#include "pty.hpp"

namespace {

const QStringList plainEnv{"PATH=/usr/bin:/bin", "TERM=xterm-256color", "LC_ALL=C"};

bool processGone(qint64 pid) { return kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH; }

// a started Pty with its output and its exit gathered
struct Run {
  Pty pty;
  QByteArray out;
  QList<int> exits;
  Run() {
    QObject::connect(&pty, &Pty::output, &pty, [this](const QByteArray &b) { out += b; });
    QObject::connect(&pty, &Pty::exited, &pty, [this](int s) { exits << s; });
  }
  bool start(const QString &prog, const QStringList &args, int rows = 24, int cols = 80,
             const QStringList &env = plainEnv) {
    return pty.start(prog, args, env, "/", rows, cols);
  }
  bool waitOut(const QByteArray &what, int ms = 5000) {
    QElapsedTimer t;
    t.start();
    while (!out.contains(what) && t.elapsed() < ms) QTest::qWait(20);
    return out.contains(what);
  }
  bool waitExit(int ms = 5000) {
    QElapsedTimer t;
    t.start();
    while (exits.isEmpty() && t.elapsed() < ms) QTest::qWait(20);
    return !exits.isEmpty();
  }
};

}  // namespace

class TestPty : public QObject {
  Q_OBJECT
private slots:
  void outputThenExitCode() {
    Run r;
    QVERIFY(r.start("/bin/sh", {"-c", "printf hello; exit 3"}));
    QVERIFY(r.pty.running());
    QVERIFY(r.waitExit());
    QCOMPARE(r.out, QByteArray("hello"));
    QCOMPARE(r.exits, QList<int>{3});
    QVERIFY(!r.pty.running());
    QTest::qWait(300);
    QCOMPARE(r.exits.size(), 1);  // once
  }
  void exitedComesAfterTheLastOutput() {
    Run r;
    QByteArray atExit;
    QObject::connect(&r.pty, &Pty::exited, &r.pty, [&] { atExit = r.out; });
    QVERIFY(r.start("/bin/sh", {"-c", "i=0; while [ $i -lt 300 ]; do echo line$i; i=$((i+1)); done"}));
    QVERIFY(r.waitExit());
    QVERIFY(atExit.contains("line299"));
  }
  void signalledChildIs128PlusSignal() {
    Run r;
    QVERIFY(r.start("/bin/sh", {"-c", "kill -9 $$"}));
    QVERIFY(r.waitExit());
    QCOMPARE(r.exits, QList<int>{137});
  }
  void catEchoesAndTheDestructorLeavesNothing() {
    qint64 pid = 0;
    {
      Run r;
      QVERIFY(r.start("/bin/cat", {}));
      pid = r.pty.pid();
      QVERIFY(pid > 0);
      r.pty.write("abc\n");
      QVERIFY(r.waitOut("abc"));
    }
    QElapsedTimer t;
    t.start();
    while (!processGone(pid) && t.elapsed() < 1000) QTest::qWait(10);
    QVERIFY(processGone(pid));
  }
  void aChildThatIgnoresHangupIsKilled() {
    qint64 pid = 0;
    auto r = std::make_unique<Run>();
    QVERIFY(r->start("/bin/sh", {"-c", "trap '' HUP; sleep 60"}));
    pid = r->pty.pid();
    QTest::qWait(300);  // the trap is set
    QElapsedTimer t;
    t.start();
    r.reset();
    QVERIFY(t.elapsed() < 1000);
    QVERIFY(processGone(pid));
    // and the sleep it started, which was in the same group, is gone with it
    QElapsedTimer waited;
    waited.start();
    bool left = true;
    while (left && waited.elapsed() < 1000) {
      QProcess ps;
      ps.start("/bin/ps", {"-axo", "pgid=,stat=,args="});
      QVERIFY(ps.waitForFinished());
      left = false;
      for (const QByteArray &line : ps.readAllStandardOutput().split('\n'))
        if (line.trimmed().startsWith(QByteArray::number(pid) + " ") && !line.contains(" Z")) left = true;
      if (left) QTest::qWait(20);
    }
    QVERIFY(!left);
  }
  void sizeAndResize() {
    Run r;
    QVERIFY(r.start("/bin/sh", {"-c", "stty size; read x; stty size; read x"}, 30, 100));
    QVERIFY(r.waitOut("30 100"));
    r.pty.resize(10, 40);
    r.pty.write("\n");
    QVERIFY(r.waitOut("10 40"));
    r.pty.write("\n");
    QVERIFY(r.waitExit());
  }
  void environmentReachesTheChild() {
    Run r;
    QVERIFY(r.start("/bin/sh", {"-c", "printf %s \"$MOTH_HOME\""}, 24, 80,
                    {"MOTH_HOME=/tmp/with space/moth", "PATH=/usr/bin:/bin"}));
    QVERIFY(r.waitExit());
    QCOMPARE(r.out, QByteArray("/tmp/with space/moth"));
  }
  void theChildLeadsItsOwnGroupOnATerminal() {
    Run r;
    QVERIFY(r.start("/bin/sh", {"-c", "[ -t 0 ] && echo tty; ps -o pid=,pgid= -p $$"}));
    QVERIFY(r.waitExit());
    QVERIFY(r.out.contains("tty"));
    const QStringList ids = QString::fromLatin1(r.out).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    QCOMPARE(ids.size(), 3);  // tty, pid, pgid: the child leads its own group
    QCOMPARE(ids[1], ids[2]);
  }
  void aProgramOnThePathIsFound() {
    Run r;
    QVERIFY(r.start("printf", {"hi"}));
    QVERIFY(r.waitExit());
    QCOMPARE(r.out, QByteArray("hi"));
  }
  void aMissingProgramReturnsFalse() {
    Run r;
    QVERIFY(!r.start("/nonexistent", {}));
    QVERIFY(!r.pty.running());
    QCOMPARE(r.pty.pid(), qint64(0));
  }
  void aMissingFolderReturnsFalse() {
    Run r;
    QVERIFY(!r.pty.start("/bin/sh", {}, plainEnv, "/nonexistent-folder", 24, 80));
    QVERIFY(!r.pty.running());
  }
  void writeWithoutAChildDoesNothing() {
    Run r;
    r.pty.write("x");
    QVERIFY(r.start("/bin/sh", {"-c", "exit 0"}));
    QVERIFY(r.waitExit());
    r.pty.write("late");
    r.pty.resize(5, 5);
    QTest::qWait(100);
    QCOMPARE(r.exits.size(), 1);
  }
  void aLargeWriteIsQueued() {
    Run r;
    QVERIFY(r.start("/bin/sh", {"-c", "stty raw -echo; head -c 200000 | wc -c"}));
    QTest::qWait(200);
    r.pty.write(QByteArray(200000, 'a'));
    QVERIFY(r.waitOut("200000", 8000));
  }
};

QTEST_MAIN(TestPty)
#include "test_pty.moc"
