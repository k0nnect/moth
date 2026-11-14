#include <QApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtTest>

#include "term_screen.hpp"
#include "terminal_view.hpp"

namespace {

const QStringList plainEnv{"PATH=/usr/bin:/bin", "TERM=xterm-256color", "LC_ALL=C"};
const QColor background(0x16, 0x16, 0x16), foreground(0xd8, 0xd8, 0xd8);

// on macOS Qt calls the Control key Meta, and Command Control
#ifdef Q_OS_MACOS
constexpr Qt::KeyboardModifier terminalControl = Qt::MetaModifier;
#else
constexpr Qt::KeyboardModifier terminalControl = Qt::ControlModifier;
#endif

// a plain interactive shell that reads none of the owner's start-up files
QStringList shellEnv(const QString &home) {
  return plainEnv + QStringList{"HOME=" + home, "SHELL=/bin/sh", "PS1=$ ", "ENV=",
                                "BASH_SILENCE_DEPRECATION_WARNING=1"};
}

QString screenText(const TerminalView &v) {
  QStringList rows;
  for (int r = 0; r < v.screen()->rows(); ++r) rows << v.screen()->text(r);
  return rows.join('\n');
}

bool shows(const TerminalView &v, const QString &text, int ms = 5000) {
  return QTest::qWaitFor([&] { return screenText(v).contains(text); }, ms);
}

// a key press and its release as macOS delivers them: the key, its modifiers and the text it
// composed. true when the press was taken
bool press(QWidget *w, int key, Qt::KeyboardModifiers mods, const QString &text) {
  QKeyEvent down(QEvent::KeyPress, key, mods, text), up(QEvent::KeyRelease, key, mods, text);
  QApplication::sendEvent(w, &down);
  QApplication::sendEvent(w, &up);
  return down.isAccepted();
}

// a program that prints the first `count` bytes it reads as od does ("1b 62"), then exits.
// at most 12 bytes, so that what od prints fits on the row
bool startDump(TerminalView &v, int count) {
  const QString script = QString("stty raw -echo; printf ready; head -c %1 | od -An -tx1").arg(count);
  return v.run("/bin/sh", {"-c", script}, plainEnv, "/") &&
         QTest::qWaitFor([&] { return v.screen()->text(0).startsWith("ready"); });
}

QString dumped(TerminalView &v) {
  if (!QTest::qWaitFor([&] { return !v.running(); })) return "(still running) " + screenText(v);
  return screenText(v).mid(5).simplified();
}

// a view with what its programs exit with gathered
struct Term {
  TerminalView view;
  QList<int> exits;
  Term() { QObject::connect(&view, &TerminalView::exited, &view, [this](int s) { exits << s; }); }
  bool run(const QString &program, const QStringList &args, const QStringList &env = plainEnv) {
    return view.run(program, args, env, "/");
  }
  bool shown(int w = 800, int h = 480) {
    view.resize(w, h);
    view.show();
    return QTest::qWaitForWindowExposed(&view);
  }
  bool focused() {
    view.activateWindow();
    if (!QTest::qWaitForWindowActive(&view)) return false;
    view.setFocus();
    return QTest::qWaitFor([&] { return view.hasFocus(); });
  }
  bool waitExit(int ms = 5000) { return QTest::qWaitFor([&] { return !exits.isEmpty(); }, ms); }
};

// another widget above a terminal, in one window
struct Pair {
  QWidget window;
  QLineEdit *edit = new QLineEdit(&window);
  TerminalView *view = new TerminalView(&window);
  bool shown() {
    auto *column = new QVBoxLayout(&window);
    column->setSizeConstraint(QLayout::SetNoConstraint);  // the offscreen platform warns of size hints
    column->addWidget(edit);
    column->addWidget(view);
    window.resize(600, 400);
    window.show();
    window.activateWindow();
    return QTest::qWaitForWindowExposed(&window) && QTest::qWaitForWindowActive(&window);
  }
};

// the view as drawn, and where its cells are in the picture (in the view's own coordinates)
struct Picture {
  QImage image;
  QPoint origin;  // the top left of cell (0, 0)
  int cw, ch, cols;
  explicit Picture(TerminalView &v)
      : image(v.grab().toImage()), origin(v.viewport()->geometry().topLeft()),
        cw(v.fontMetrics().averageCharWidth()), ch(v.fontMetrics().lineSpacing()),
        cols(v.screen()->cols()) {}
  QRect cells(int row, int col, int n = 1) const {
    return QRect(origin.x() + col * cw, origin.y() + row * ch, n * cw, ch);
  }
  QRect row(int r) const { return cells(r, 0, cols); }
  QColor colour(QPoint p) const {
    const qreal dpr = image.devicePixelRatio();
    return image.pixelColor(qFloor(p.x() * dpr), qFloor(p.y() * dpr));
  }
  int countNot(const QRect &r, const QColor &c) const {
    const qreal dpr = image.devicePixelRatio();
    int n = 0;
    for (int y = qFloor(r.top() * dpr); y < qFloor((r.bottom() + 1) * dpr); ++y)
      for (int x = qFloor(r.left() * dpr); x < qFloor((r.right() + 1) * dpr); ++x)
        if (image.pixelColor(x, y) != c) ++n;
    return n;
  }
};

// a view that remembers what it was asked to paint
struct Probe : TerminalView {
  QRegion painted;
  void paintEvent(QPaintEvent *e) override {
    painted += e->region();
    TerminalView::paintEvent(e);
  }
};

}  // namespace

class TestTerminalView : public QObject {
  Q_OBJECT
private slots:
  void printedTextIsOnTheScreenAndPainted() {
    Term t;
    QVERIFY(t.shown());
    QVERIFY(t.run("/bin/sh", {"-c", "printf 'hello\\n'; sleep 5"}));
    QVERIFY(QTest::qWaitFor([&] { return t.view.screen()->text(0) == "hello"; }, 2000));
    const Picture pic(t.view);
    QVERIFY(pic.countNot(pic.row(0), background) > 0);
    QCOMPARE(pic.countNot(pic.row(5), background), 0);
  }
  void typedKeysReachTheProgram() {
    Term t;
    QVERIFY(t.run("/bin/cat", {}));
    QTest::keyClick(&t.view, Qt::Key_A);
    QTest::keyClick(&t.view, Qt::Key_B);
    QTest::keyClick(&t.view, Qt::Key_Return);
    QVERIFY(QTest::qWaitFor([&] { return t.view.screen()->text(1) == "ab"; }));  // cat's copy
    QCOMPARE(t.view.screen()->text(0), QString("ab"));                         // the echo
  }
  void inputMethodTextIsSentAndCompositionIsNot() {
    Term t;
    QVERIFY(t.shown());
    QVERIFY(t.focused());
    QVERIFY(t.run("/bin/cat", {}));
    QInputMethodEvent compose("abcd", {});
    QApplication::sendEvent(&t.view, &compose);
    QTest::qWait(300);
    QCOMPARE(t.view.screen()->text(0), QString());  // nothing reached the pty, so nothing echoed
    const Picture composing(t.view);
    QVERIFY(composing.countNot(composing.cells(0, 2, 2), background) > 0);  // "cd", at the cursor
    QInputMethodEvent commit;
    commit.setCommitString("é");
    QApplication::sendEvent(&t.view, &commit);
    QVERIFY(QTest::qWaitFor([&] { return t.view.screen()->text(0) == "é"; }));
    const Picture done(t.view);
    QCOMPARE(done.countNot(done.cells(0, 2, 2), background), 0);  // the composition is gone
  }
  void theInputMethodCanReplaceWhatWasJustTyped() {
    Term t;
    QVERIFY(t.run("/bin/cat", {}));
    QTest::keyClick(&t.view, Qt::Key_E);
    QVERIFY(QTest::qWaitFor([&] { return t.view.screen()->text(0) == "e"; }));
    // the press-and-hold menu: é in place of the e it was opened on
    QInputMethodEvent accent;
    accent.setCommitString("é", -1, 1);
    QApplication::sendEvent(&t.view, &accent);
    QTest::keyClick(&t.view, Qt::Key_Return);
    QVERIFY(QTest::qWaitFor([&] { return t.view.screen()->text(1) == "é"; }));  // what cat read
    QCOMPARE(t.view.screen()->text(0), QString("é"));
  }
  void theInputMethodTakesBackOnlyWhatTheViewSent() {
    Term t;
    auto replace = [&](const QString &commit, int start, int length, const QString &preedit = "") {
      QInputMethodEvent e(preedit, {});
      e.setCommitString(commit, start, length);
      QApplication::sendEvent(&t.view, &e);
    };
    QVERIFY(startDump(t.view, 12));
    replace("é", -1, 1);                                     // "ready" is the program's: c3 a9
    press(&t.view, Qt::Key_X, Qt::NoModifier, "x");          // 78
    press(&t.view, Qt::Key_Left, Qt::KeypadModifier, "");    // 1b 5b 44, and the x is forgotten
    replace("é", -1, 1);                                     // c3 a9
    press(&t.view, Qt::Key_E, Qt::NoModifier, "e");          // 65
    replace("é", -1, 1);                                     // 7f c3 a9: the e is taken back
    QCOMPARE(dumped(t.view), QString("c3 a9 78 1b 5b 44 c3 a9 65 7f c3 a9"));
    QVERIFY(startDump(t.view, 7));
    press(&t.view, Qt::Key_E, Qt::NoModifier, "e");          // 65
    replace("é", -2, 2);                                     // more than was sent: c3 a9 alone
    press(&t.view, Qt::Key_A, Qt::NoModifier, "a");          // 61
    replace("", -1, 1, "a");                                 // 7f: taken back into a composition
    replace("à", 0, 0);                                      // c3 a0
    QCOMPARE(dumped(t.view), QString("65 c3 a9 61 7f c3 a0"));
  }
  void theInputMethodIsToldWhereTheCursorGoes() {
    Term t;
    QVERIFY(t.shown());
    QVERIFY(t.focused());
    QTest::qWait(50);
    QSignalSpy told(QGuiApplication::inputMethod(), &QInputMethod::cursorRectangleChanged);
    t.view.screen()->feed("abc");  // the cursor moves
    QVERIFY(QTest::qWaitFor([&] { return told.count() == 1; }));
    QInputMethodEvent compose("x", {});  // the composition changes
    QApplication::sendEvent(&t.view, &compose);
    QCOMPARE(told.count(), 2);
    t.view.screen()->feed("\x1b[5;1Hy\x1b[1;4H");  // a row changes, the cursor ends where it was
    QTest::qWait(50);
    QCOMPARE(told.count(), 2);
    t.view.clearFocus();
    QTest::qWait(50);
    told.clear();
    t.view.screen()->feed("def");  // without the focus it is not told
    QTest::qWait(50);
    QCOMPARE(told.count(), 0);
  }
  void inputMethodQueries() {
    Term t;
    QVERIFY(t.shown());
    QVERIFY(t.focused());
    t.view.screen()->feed("abc");
    QVERIFY(t.view.inputMethodQuery(Qt::ImEnabled).toBool());
    const Picture pic(t.view);
    QCOMPARE(t.view.inputMethodQuery(Qt::ImCursorRectangle).toRect(), pic.cells(0, 3));
    const auto hints = Qt::InputMethodHints(t.view.inputMethodQuery(Qt::ImHints).toInt());
    QVERIFY(hints & Qt::ImhNoAutoUppercase);
    QVERIFY(hints & Qt::ImhNoPredictiveText);
    QCOMPARE(t.view.inputMethodQuery(Qt::ImSurroundingText).toString(), QString("abc"));
    QCOMPARE(t.view.inputMethodQuery(Qt::ImCursorPosition).toInt(), 3);
    // while Option is held macOS must not compose with it: Option+e is ESC e, not an accent
    QTest::keyPress(t.view.windowHandle(), Qt::Key_Alt, Qt::AltModifier);
    QVERIFY(!t.view.inputMethodQuery(Qt::ImEnabled).toBool());
    QTest::keyRelease(t.view.windowHandle(), Qt::Key_Alt, Qt::NoModifier);
    QVERIFY(t.view.inputMethodQuery(Qt::ImEnabled).toBool());
  }
  void aWiderViewHasMoreColumnsAndTheProgramIsTold() {
    QTemporaryDir home;
    QVERIFY(home.isValid());
    Term t;
    QVERIFY(t.shown(400, 300));
    const int cols = t.view.screen()->cols();
    QVERIFY(cols > 10);
    QVERIFY(t.run("/bin/sh", {}, shellEnv(home.path())));
    QVERIFY(shows(t.view, "$"));
    // the viewport twice as wide: the scroll bar beside it keeps its width
    t.view.resize(t.view.width() + t.view.viewport()->width(), t.view.height());
    QVERIFY(QTest::qWaitFor([&] { return std::abs(t.view.screen()->cols() - 2 * cols) <= 1; }));
    const QString size = QString("%1 %2").arg(t.view.screen()->rows()).arg(t.view.screen()->cols());
    QTest::keyClicks(&t.view, "stty size");
    QTest::keyClick(&t.view, Qt::Key_Return);
    QVERIFY2(shows(t.view, "\n" + size), qPrintable(screenText(t.view)));
  }
  void theSizeIsTwentyFourByEightyUntilShownAndNeverUnderTwoByTen() {
    Term t;
    QCOMPARE(t.view.screen()->rows(), 24);
    QCOMPARE(t.view.screen()->cols(), 80);
    QVERIFY(t.run("/bin/sh", {"-c", "stty size; sleep 5"}));
    QVERIFY(shows(t.view, "24 80"));
    QVERIFY(t.shown(800, 480));
    const QSize area = t.view.viewport()->size();
    QCOMPARE(t.view.screen()->rows(), area.height() / t.view.fontMetrics().lineSpacing());
    QCOMPARE(t.view.screen()->cols(), area.width() / t.view.fontMetrics().averageCharWidth());
    TerminalView tiny;
    tiny.resize(30, 20);
    tiny.show();
    QVERIFY(QTest::qWaitForWindowExposed(&tiny));
    QCOMPARE(tiny.screen()->rows(), 2);
    QCOMPARE(tiny.screen()->cols(), 10);
  }
  void aFloodIsPaintedInFewFrames() {
    Term t;
    QVERIFY(t.shown());
    QTest::qWait(100);
    // damage is gathered for 16 ms: a burst of a few milliseconds, with the event loop turning
    // between its pieces, is painted at most once while it lasts and once when the time is up
    const int burst = t.view.paints();
    for (int i = 0; i < 100; ++i) {
      t.view.screen()->feed("x");
      QCoreApplication::processEvents();
    }
    const int during = t.view.paints() - burst;
    QVERIFY2(during <= 1, qPrintable(QString::number(during)));
    QTest::qWait(50);
    QCOMPARE(t.view.paints() - burst - during, 1);
    // and a real flood, from a program
    const int before = t.view.paints();
    QVERIFY(t.run("/bin/sh", {"-c", "i=0; while [ $i -lt 5000 ]; do printf 'line %d\\n' $i; i=$((i+1)); done; sleep 5"}));
    QVERIFY(shows(t.view, "line 4999", 10000));
    QTest::qWait(100);  // the last of it painted
    const int painted = t.view.paints() - before;
    QVERIFY2(painted >= 1 && painted < 400, qPrintable(QString::number(painted)));
  }
  void onlyDamagedRowsArePainted() {
    Probe v;
    v.resize(800, 480);
    v.show();
    QVERIFY(QTest::qWaitForWindowExposed(&v));
    v.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&v));
    v.setFocus();
    v.screen()->feed("\x1b[5;1H");  // the cursor to row 4
    QTest::qWait(100);
    const int ch = v.fontMetrics().lineSpacing();
    const QRegion row4(QRect(0, 4 * ch, v.viewport()->width(), ch));
    v.painted = QRegion();
    v.screen()->feed("row four");
    QVERIFY(QTest::qWaitFor([&] { return !v.painted.isEmpty(); }));
    QTest::qWait(50);
    QCOMPARE(v.painted - row4, QRegion());  // what Qt was asked to repaint
    QCOMPARE(v.rowsPainted(), 1);           // and what was drawn
    // losing the focus repaints the cursor's row, and only that
    v.painted = QRegion();
    v.clearFocus();
    QVERIFY(QTest::qWaitFor([&] { return !v.painted.isEmpty(); }));
    QTest::qWait(50);
    QCOMPARE(v.painted - row4, QRegion());
    QCOMPARE(v.rowsPainted(), 1);
    // a whole repaint draws every row
    v.viewport()->repaint();
    QCOMPARE(v.rowsPainted(), v.screen()->rows());
  }
  void theCursorIsABlockWhenFocusedAnOutlineWhenNotAndHiddenWhenAsked() {
    Term t;
    QVERIFY(t.shown());
    QVERIFY(t.focused());
    t.view.screen()->feed("ab");
    const Picture block(t.view);
    const QRect cell = block.cells(0, 2);
    QCOMPARE(block.colour(cell.center()), foreground);
    t.view.clearFocus();
    QVERIFY(QTest::qWaitFor([&] { return !t.view.hasFocus(); }));
    const Picture outline(t.view);
    QCOMPARE(outline.colour(cell.center()), background);
    QCOMPARE(outline.colour(cell.topLeft()), foreground);
    QCOMPARE(outline.colour(cell.bottomRight()), foreground);
    t.view.screen()->feed("\x1b[?25l");
    const Picture hidden(t.view);
    QCOMPARE(hidden.countNot(cell, background), 0);
  }
  void coloursAndReverseVideo() {
    Term t;
    QVERIFY(t.shown());
    // red background; the default reversed; blue on green; red on green reversed
    t.view.screen()->feed("\x1b[41m  \x1b[0m\x1b[7m  \x1b[0m\x1b[38;2;0;0;255;48;2;0;255;0m  \x1b[0m"
                          "\x1b[7;31;42m  \x1b[0m\r\n");
    const Picture pic(t.view);
    QCOMPARE(pic.colour(pic.cells(0, 0).center()), QColor(205, 0, 0));
    QCOMPARE(pic.colour(pic.cells(0, 2).center()), foreground);
    QCOMPARE(pic.colour(pic.cells(0, 4).center()), QColor(0, 255, 0));
    QCOMPARE(pic.colour(pic.cells(0, 6).center()), QColor(205, 0, 0));
    QCOMPARE(pic.colour(pic.cells(0, 8).center()), background);
  }
  void wideAndFallbackCharactersStayOnTheGrid() {
    Term t;
    QVERIFY(t.shown());
    // braille, ⎿ and ✻ are not in Menlo: they come from other fonts, with other advances
    t.view.screen()->feed(QString("日本x\r\n⠿⠿⠿⠿⠿⠿⠿⠿⠿⠿x\r\na⎿b✻c⠿d日e\r\nab日本テキ|x\r\n").toUtf8());
    const Picture pic(t.view);
    QVERIFY(pic.countNot(pic.cells(0, 4), background) > 0);
    QCOMPARE(pic.countNot(pic.cells(0, 5, 10), background), 0);
    QVERIFY(pic.countNot(pic.cells(1, 9), background) > 0);
    QVERIFY(pic.countNot(pic.cells(1, 10), background) > 0);
    QCOMPARE(pic.countNot(pic.cells(1, 11, 10), background), 0);
    // Menlo's letters between the others, each in its own cell
    const QList<QPair<int, QString>> letters{{0, "a"}, {2, "b"}, {4, "c"}, {6, "d"}, {9, "e"}};
    for (const auto &[col, letter] : letters) {
      QCOMPARE(t.view.screen()->cell(2, col).text, letter);
      QVERIFY2(pic.countNot(pic.cells(2, col), background) > 0, qPrintable(letter));
    }
    QCOMPARE(pic.countNot(pic.cells(2, 10, 10), background), 0);
    // and Menlo again after two other fonts (Qt's glyph runs lose track of which cell is which here)
    QCOMPARE(t.view.screen()->cell(3, 10).text, QString("|"));
    QVERIFY(pic.countNot(pic.cells(3, 10), background) > 0);
    QVERIFY(pic.countNot(pic.cells(3, 11), background) > 0);
    QCOMPARE(pic.countNot(pic.cells(3, 12, 10), background), 0);
  }
  void keysForAnotherWidgetReachNoProgram() {
    Pair p;
    QVERIFY(p.shown());
    QVERIFY(p.view->run("/bin/cat", {}, plainEnv, "/"));
    p.edit->setFocus();
    QTest::keyClicks(p.edit, "xyz");
    for (char c : QByteArray("uvw")) QTest::keyClick(p.window.windowHandle(), c);  // to the focus
    QTest::keyClick(p.window.windowHandle(), Qt::Key_Return);
    QTest::qWait(300);
    QCOMPARE(p.edit->text(), QString("xyzuvw"));
    QCOMPARE(screenText(*p.view).trimmed(), QString());
    // the same way in, with the focus on the view, reaches the program
    p.view->setFocus();
    QTest::keyClick(p.window.windowHandle(), 'q');
    QVERIFY(QTest::qWaitFor([&] { return p.view->screen()->text(0) == "q"; }));
  }
  void controlAndOptionSendWhatATerminalSends() {
    Term t;
    QVERIFY(startDump(t.view, 9));
    QTest::keyClick(&t.view, Qt::Key_C, terminalControl);                   // 03
    press(&t.view, Qt::Key_B, Qt::AltModifier, "∫");                          // 1b 62, not ∫
    press(&t.view, Qt::Key_B, Qt::AltModifier | Qt::ShiftModifier, "ı");      // 1b 42
    press(&t.view, Qt::Key_1, Qt::AltModifier, "¡");                          // 1b 31
    press(&t.view, Qt::Key_Left, Qt::AltModifier | Qt::KeypadModifier, "");   // 1b 62
    QCOMPARE(dumped(t.view), QString("03 1b 62 1b 42 1b 31 1b 62"));
    // past ASCII: Option+ö on a German keyboard, and a key Qt has no code for
    QVERIFY(startDump(t.view, 6));
    press(&t.view, 0xd6, Qt::AltModifier, "œ");                               // 1b c3 b6
    press(&t.view, Qt::Key_unknown, Qt::AltModifier, "π");                    // 1b cf 80
    QCOMPARE(dumped(t.view), QString("1b c3 b6 1b cf 80"));
  }
  void commandKeysAreNotSent() {
#ifndef Q_OS_MACOS
    QSKIP("Command is a Mac key");
#endif
    Term t;
    QVERIFY(startDump(t.view, 1));
    // Command is Qt's Control on macOS. C, V and A are the view's (copy, paste, select all)
    QVERIFY(press(&t.view, Qt::Key_C, Qt::ControlModifier, "c"));
    QVERIFY(press(&t.view, Qt::Key_V, Qt::ControlModifier, "v"));
    QVERIFY(press(&t.view, Qt::Key_A, Qt::ControlModifier, "a"));
    QVERIFY(!press(&t.view, Qt::Key_Q, Qt::ControlModifier, "q"));  // left for the application
    // and so is any chord of Command with other modifiers, even on C, V or A
    QVERIFY(!press(&t.view, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier, "C"));
    QVERIFY(!press(&t.view, Qt::Key_V, Qt::ControlModifier | Qt::AltModifier, "√"));
    QVERIFY(!press(&t.view, Qt::Key_A, Qt::ControlModifier | Qt::MetaModifier, "\x01"));
    // and the application's shortcuts may have Command, but no other key the program wants
    QKeyEvent quit(QEvent::ShortcutOverride, Qt::Key_Q, Qt::ControlModifier);
    quit.ignore();
    QApplication::sendEvent(&t.view, &quit);
    QVERIFY(!quit.isAccepted());
    QKeyEvent copyShifted(QEvent::ShortcutOverride, Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
    copyShifted.ignore();
    QApplication::sendEvent(&t.view, &copyShifted);
    QVERIFY(!copyShifted.isAccepted());
    QKeyEvent copy(QEvent::ShortcutOverride, Qt::Key_C, Qt::ControlModifier);
    copy.ignore();
    QApplication::sendEvent(&t.view, &copy);
    QVERIFY(copy.isAccepted());
    QKeyEvent escape(QEvent::ShortcutOverride, Qt::Key_Escape, Qt::NoModifier);
    escape.ignore();
    QApplication::sendEvent(&t.view, &escape);
    QVERIFY(escape.isAccepted());
    press(&t.view, Qt::Key_Z, Qt::NoModifier, "z");
    QCOMPARE(dumped(t.view), QString("7a"));
  }
  void tabGoesToTheProgram() {
    Pair p;
    QVERIFY(p.shown());
    QVERIFY(startDump(*p.view, 4));
    p.view->setFocus();
    QTest::keyClick(p.window.windowHandle(), Qt::Key_Tab);
    QTest::keyClick(p.window.windowHandle(), Qt::Key_Backtab, Qt::ShiftModifier);
    QCOMPARE(dumped(*p.view), QString("09 1b 5b 5a"));
    QVERIFY(p.view->hasFocus());
  }
  void runWhileRunningIsRefusedAndAfterTheExitStartsClean() {
    Term t;
    QVERIFY(!t.run("/nonexistent/program", {}));
    QVERIFY(!t.view.running());
    QVERIFY(t.run("/bin/sh", {"-c", "i=0; while [ $i -lt 30 ]; do echo line $i; i=$((i+1)); done;"
                                    " printf '\\033]2;old\\007\\033[?1049h\\033[Hother'; read x"}));
    TermScreen *s = t.view.screen();
    QVERIFY(QTest::qWaitFor([&] { return s->text(0) == "other"; }));
    QVERIFY(s->altScreen());
    QVERIFY(s->scrollback() > 0);
    QCOMPARE(s->title(), QString("old"));
    const qint64 pid = t.view.pid();
    QVERIFY(pid > 0);
    QVERIFY(!t.run("/bin/sh", {"-c", "printf second"}));
    QCOMPARE(t.view.pid(), pid);
    QTest::keyClick(&t.view, Qt::Key_Return);
    QVERIFY(t.waitExit());
    QCOMPARE(s->text(0), QString("other"));  // the last screen stays
    QVERIFY(!t.run("/nonexistent/program", {}));
    QCOMPARE(s->text(0), QString("other"));  // and stays when nothing could be started
    QVERIFY(t.run("/bin/sh", {"-c", "printf second; sleep 5"}));
    QCOMPARE(t.view.screen(), s);
    QCOMPARE(s->text(0), QString());
    QVERIFY(!s->altScreen());
    QCOMPARE(s->scrollback(), 0);
    QCOMPARE(s->title(), QString());
    QVERIFY(QTest::qWaitFor([&] { return s->text(0) == "second"; }));
  }
  void theLastScreenStaysWhenTheProgramExits() {
    Term t;
    QVERIFY(t.run("/bin/sh", {"-c", "printf 'goodbye\\n'; exit 3"}));
    QVERIFY(t.waitExit());
    QCOMPARE(t.exits, QList<int>{3});
    QCOMPARE(t.view.screen()->text(0), QString("goodbye"));
    QVERIFY(!t.view.running());
    QCOMPARE(t.view.pid(), qint64(0));
  }
};

QTEST_MAIN(TestTerminalView)
#include "test_terminal_view.moc"
