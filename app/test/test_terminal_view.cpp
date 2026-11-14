#include <QApplication>
#include <QClipboard>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QMimeData>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtTest>
#include <cerrno>
#include <csignal>

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

// the same for more bytes, with no control character but ESC (mouse reports, arrow keys), after
// the shell commands `first`: only echo and line editing are turned off, and the bytes are printed
// eight to a line, each line on a row of its own
bool startLongDump(TerminalView &v, int count, const QString &first) {
  const QString script = first + "; stty -echo -icanon; printf ready; head -c " + QString::number(count) +
                         " | hexdump -v -e '8/1 \" %02x\" \"\\n\"'";
  return v.run("/bin/sh", {"-c", script}, plainEnv, "/") &&
         QTest::qWaitFor([&] { return v.screen()->text(0).startsWith("ready"); });
}

// a dump of mouse reports, after asking for them (1000: presses and releases, 1002: drags too)
bool startMouseDump(TerminalView &v, int count, const QString &mode) {
  return startLongDump(v, count, "printf '\\033[?" + mode + "h\\033[?1006h'") && v.screen()->mouseWanted();
}

QString dumped(TerminalView &v) {
  if (!QTest::qWaitFor([&] { return !v.running(); })) return "(still running) " + screenText(v);
  return screenText(v).mid(5).simplified();
}

// numbered lines, "line 1" to "line <last>", each ended
QByteArray numberedLines(int first, int last) {
  QByteArray out;
  for (int i = first; i <= last; ++i) out += "line " + QByteArray::number(i) + "\r\n";
  return out;
}

int cellWidth(const TerminalView &v) { return v.fontMetrics().averageCharWidth(); }
int cellHeight(const TerminalView &v) { return v.fontMetrics().lineSpacing(); }

// the middle of a cell, in the viewport's coordinates (where mouse events are delivered)
QPoint at(const TerminalView &v, int row, int col) {
  return QPoint(col * cellWidth(v) + cellWidth(v) / 2, row * cellHeight(v) + cellHeight(v) / 2);
}

// the text of the row at the top of the view: the scroll bar's value counts lines from the oldest
QString topRow(const TerminalView &v) {
  const QScrollBar *bar = v.verticalScrollBar();
  return v.screen()->text(bar->value() - bar->maximum());
}

// a mouse event of the left button, as Qt delivers it to the viewport
void mouse(TerminalView &v, QEvent::Type type, QPoint p, Qt::KeyboardModifiers mods = Qt::NoModifier) {
  const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
  const Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
  QMouseEvent e(type, QPointF(p), v.viewport()->mapToGlobal(QPointF(p)), button, held, mods);
  QApplication::sendEvent(v.viewport(), &e);
}

void drag(TerminalView &v, QPoint from, QPoint to, Qt::KeyboardModifiers mods = Qt::NoModifier) {
  mouse(v, QEvent::MouseButtonPress, from, mods);
  mouse(v, QEvent::MouseMove, to, mods);
  mouse(v, QEvent::MouseButtonRelease, to, mods);
}

// a double click as Qt delivers it: the second press comes as a double click
void doubleClick(TerminalView &v, QPoint p) {
  mouse(v, QEvent::MouseButtonPress, p);
  mouse(v, QEvent::MouseButtonRelease, p);
  mouse(v, QEvent::MouseButtonDblClick, p);
  mouse(v, QEvent::MouseButtonRelease, p);
}

void sendWheel(TerminalView &v, QPoint p, QPoint pixels, QPoint angle, Qt::ScrollPhase phase) {
  QWheelEvent e(QPointF(p), v.viewport()->mapToGlobal(QPointF(p)), pixels, angle, Qt::NoButton,
                Qt::NoModifier, phase, false);
  QApplication::sendEvent(v.viewport(), &e);
}

// a mouse wheel's notches, up positive, as macOS's Qt delivers them: 120 each, and pixels too (20
// for each line macOS would scroll), with no scroll phase
void wheel(TerminalView &v, QPoint p, int notches) {
  sendWheel(v, p, QPoint(0, 20 * notches), QPoint(0, 120 * notches), Qt::NoScrollPhase);
}

// a trackpad's move, up positive: pixels, in a scroll phase (and Qt's angle for them, two a pixel)
void trackpad(TerminalView &v, QPoint p, int pixels) {
  sendWheel(v, p, QPoint(0, pixels), QPoint(0, 2 * pixels), Qt::ScrollUpdate);
}

// all the bytes a screen sent to its program while a spy watched
QByteArray sentBy(const QSignalSpy &spy) {
  QByteArray all;
  for (const QList<QVariant> &args : spy) all += args.at(0).toByteArray();
  return all;
}

// the clipboard as it was, put back when the test ends
struct KeptClipboard {
  QMimeData *kept = new QMimeData;
  KeptClipboard() {
    if (const QMimeData *now = QGuiApplication::clipboard()->mimeData())
      for (const QString &format : now->formats()) kept->setData(format, now->data(format));
  }
  ~KeptClipboard() { QGuiApplication::clipboard()->setMimeData(kept); }  // which takes it
};

constexpr Qt::KeyboardModifier command = Qt::ControlModifier;  // on macOS Qt's Control is Command

// presses Command and a letter; true when the view took it
bool commandKey(TerminalView &v, Qt::Key key) {
  return press(&v, key, command, QString(QChar(static_cast<char16_t>(key))).toLower());
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
  QImage area(const QRect &r) const {
    const qreal dpr = image.devicePixelRatio();
    return image.copy(QRect(qFloor(r.left() * dpr), qFloor(r.top() * dpr), qCeil(r.width() * dpr),
                            qCeil(r.height() * dpr)));
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
  void theInputMethodNeverTakesBackHalfACharacter() {
    Term t;
    auto replace = [&](int start, int length) {
      QInputMethodEvent e;
      e.setCommitString("é", start, length);
      QApplication::sendEvent(&t.view, &e);
    };
    const QString grin = QString::fromUcs4(U"\U0001F600");  // two UTF-16 units
    QVERIFY(startDump(t.view, 6));
    press(&t.view, Qt::Key_unknown, Qt::NoModifier, grin);  // f0 9f 98 80
    replace(-1, 1);                                          // half of it: c3 a9 alone
    QCOMPARE(dumped(t.view), QString("f0 9f 98 80 c3 a9"));
    QVERIFY(startDump(t.view, 7));
    press(&t.view, Qt::Key_unknown, Qt::NoModifier, grin);  // f0 9f 98 80
    replace(-2, 2);                                          // all of it: 7f c3 a9
    QCOMPARE(dumped(t.view), QString("f0 9f 98 80 7f c3 a9"));
  }
  void theInputMethodIsToldAtOnceWhenAResizeMovesTheCursor() {
    QWidget window;
    auto *view = new TerminalView(&window);  // a child: its resizes come at once
    view->setGeometry(0, 0, 600, 400);
    window.resize(600, 400);
    window.show();
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    view->setFocus();
    QVERIFY(QTest::qWaitFor([&] { return view->hasFocus(); }));
    view->screen()->feed("\x1b[20;5H");
    QTest::qWait(50);
    QSignalSpy told(QGuiApplication::inputMethod(), &QInputMethod::cursorRectangleChanged);
    view->resize(600, 200);  // fewer rows: the cursor's row moves up
    QCOMPARE(told.count(), 1);
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
    // damage is gathered for 16 ms: a burst, with the event loop turning between its pieces, is
    // painted at most once for every 16 ms it lasts (and so once at most when it takes a few), and
    // once when the time is up
    const int burst = t.view.paints();
    QElapsedTimer clock;
    clock.start();
    for (int i = 0; i < 100; ++i) {
      t.view.screen()->feed("x");
      QCoreApplication::processEvents();
    }
    const qint64 lasted = clock.elapsed();  // on a busy machine the burst may outlast 16 ms
    const int during = t.view.paints() - burst;
    QVERIFY2(during <= lasted / 16 + 1, qPrintable(QString("%1 paints in %2 ms").arg(during).arg(lasted)));
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
    KeptClipboard kept;
    QGuiApplication::clipboard()->clear();  // so that the paste sends nothing
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

  // --- looking back, selecting, copying, the mouse

  void theScrollBarSpansTheScrollbackAndTheViewStaysWhereItIsPut() {
    Term t;
    QVERIFY(t.shown(800, 24 * cellHeight(t.view) + 8));
    QCOMPARE(t.view.screen()->rows(), 24);
    QScrollBar *bar = t.view.verticalScrollBar();
    t.view.screen()->feed(numberedLines(1, 200));
    QVERIFY2(bar->maximum() >= 176, qPrintable(QString::number(bar->maximum())));
    QCOMPARE(bar->value(), bar->maximum());  // at the bottom
    QCOMPARE(bar->pageStep(), 24);
    QCOMPARE(topRow(t.view), QString("line 178"));
    const QPoint middle = t.view.viewport()->rect().center();
    wheel(t.view, middle, 1);  // a notch up: three lines back
    QCOMPARE(topRow(t.view), QString("line 175"));
    QCOMPARE(bar->value(), bar->maximum() - 3);
    const Picture back(t.view);
    // new output leaves the top row where it is, in the scroll bar and as drawn
    t.view.screen()->feed(numberedLines(201, 210));
    QCOMPARE(topRow(t.view), QString("line 175"));
    QCOMPARE(bar->value(), bar->maximum() - 13);
    const Picture after(t.view);
    QCOMPARE(after.area(after.row(0)), back.area(back.row(0)));
    QCOMPARE(after.area(after.row(23)), back.area(back.row(23)));
    // the scroll bar moves the view
    bar->setValue(0);
    QCOMPARE(topRow(t.view), QString("line 1"));
    bar->setValue(bar->maximum() - 3);
    QCOMPARE(topRow(t.view), QString("line 185"));
    // a trackpad: its pixels, a line for each cell's height, the rest kept for the next
    trackpad(t.view, middle, cellHeight(t.view) / 2 + 1);
    QCOMPARE(bar->value(), bar->maximum() - 3);
    trackpad(t.view, middle, cellHeight(t.view) / 2 + 1);
    QCOMPARE(bar->value(), bar->maximum() - 4);
    // Command+C is the view's own: the view stays. A key for the program brings it to the bottom
    QVERIFY(commandKey(t.view, Qt::Key_C));
    QCOMPARE(bar->value(), bar->maximum() - 4);
    QTest::keyClick(&t.view, Qt::Key_A);
    QCOMPARE(bar->value(), bar->maximum());
    QCOMPARE(topRow(t.view), t.view.screen()->text(0));
  }
  void aDragSelectsCellsAcrossRowsAndCommandCCopiesThem() {
    KeptClipboard kept;
    Term t;
    QVERIFY(t.shown());
    t.view.screen()->feed("first row\r\nsecond row\r\nthird row");
    QGuiApplication::clipboard()->setText("before");
    QVERIFY(commandKey(t.view, Qt::Key_C));  // nothing selected: nothing copied
    QCOMPARE(QGuiApplication::clipboard()->text(), QString("before"));
    drag(t.view, at(t.view, 1, 0), at(t.view, 2, 3));
    QCOMPARE(t.view.selectedText(), QString("second row\nthir"));
    QVERIFY(commandKey(t.view, Qt::Key_C));
    QCOMPARE(QGuiApplication::clipboard()->text(), QString("second row\nthir"));
    QCOMPARE(t.view.selectedText(), QString("second row\nthir"));  // still selected
    // drawn reversed: the background of what is selected is the text's colour
    const Picture pic(t.view);
    QCOMPARE(pic.colour(pic.cells(1, 0).topLeft()), foreground);
    QCOMPARE(pic.colour(pic.cells(1, 30).topLeft()), foreground);  // on to the end of the row
    QCOMPARE(pic.colour(pic.cells(2, 3).topLeft()), foreground);
    QCOMPARE(pic.colour(pic.cells(2, 4).topLeft()), background);
    QCOMPARE(pic.colour(pic.cells(0, 5).topLeft()), background);
    // a click drops it
    mouse(t.view, QEvent::MouseButtonPress, at(t.view, 0, 0));
    mouse(t.view, QEvent::MouseButtonRelease, at(t.view, 0, 0));
    QCOMPARE(t.view.selectedText(), QString());
  }
  void aDoubleClickSelectsAWordAndATripleClickTheRow() {
    Term t;
    QVERIFY(t.shown());
    t.view.screen()->feed("hello world\r\n");
    doubleClick(t.view, at(t.view, 0, 8));
    QCOMPARE(t.view.selectedText(), QString("world"));
    mouse(t.view, QEvent::MouseButtonPress, at(t.view, 0, 8));  // a third press, soon after
    mouse(t.view, QEvent::MouseButtonRelease, at(t.view, 0, 8));
    QCOMPARE(t.view.selectedText(), QString("hello world"));
  }
  void aLineThatWrappedCopiesAsOneLine() {
    Term t;
    QVERIFY(t.shown());
    t.view.resize(t.view.width() - t.view.viewport()->width() + 40 * cellWidth(t.view), t.view.height());
    QVERIFY(QTest::qWaitFor([&] { return t.view.screen()->cols() == 40; }));
    QString hundred;
    for (int i = 0; i < 10; ++i) hundred += "0123456789";
    // and a line where 日 did not fit in the last column and went on to the next row
    t.view.screen()->feed((hundred + "\r\n" + QString(39, 'a') + "日 next\r\n").toUtf8());
    drag(t.view, at(t.view, 0, 0), at(t.view, 2, 19));
    QCOMPARE(t.view.selectedText(), hundred);
    doubleClick(t.view, at(t.view, 1, 5));  // a word goes on across the rows too
    QCOMPARE(t.view.selectedText(), hundred);
    drag(t.view, at(t.view, 3, 0), at(t.view, 4, 1));
    QCOMPARE(t.view.selectedText(), QString(39, 'a') + "日");
  }
  void commandVPastesTheClipboard() {
    KeptClipboard kept;
    Term t;
    QVERIFY(t.run("/bin/cat", {}));
    QGuiApplication::clipboard()->setText("abc");
    QVERIFY(commandKey(t.view, Qt::Key_V));
    QVERIFY(QTest::qWaitFor([&] { return t.view.screen()->text(0) == "abc"; }));
  }
  void commandASelectsEverythingTheScrollbackToo() {
    Term t;
    t.view.screen()->feed(numberedLines(1, 50));
    QVERIFY(t.view.screen()->scrollback() > 0);
    QVERIFY(commandKey(t.view, Qt::Key_A));
    const QString all = t.view.selectedText();
    QVERIFY(all.startsWith("line 1\nline 2\n"));
    QVERIFY(all.endsWith("\nline 49\nline 50"));  // and no empty row after
  }
  void aProgramThatAsksForTheMouseGetsItAndOptionSelects() {
    Term t;
    QVERIFY(t.shown());
    QVERIFY(startMouseDump(t.view, 28, "1000"));
    drag(t.view, at(t.view, 0, 1), at(t.view, 0, 3));  // a press and a release
    QCOMPARE(t.view.selectedText(), QString());
    drag(t.view, at(t.view, 0, 1), at(t.view, 0, 3), Qt::AltModifier);  // the view's: nothing sent
    QCOMPARE(t.view.selectedText(), QString("ead"));
    wheel(t.view, at(t.view, 2, 4), 1);  // a notch up, at row 2 column 4
    QCOMPARE(dumped(t.view), QString("1b 5b 3c 30 3b 32 3b 31 4d 1b 5b 3c 30 3b 34 3b 31 6d "
                                     "1b 5b 3c 36 34 3b 35 3b 33 4d"));
    // a program that asks for drags gets them too
    QVERIFY(startMouseDump(t.view, 28, "1002"));
    drag(t.view, at(t.view, 0, 1), at(t.view, 1, 3));
    QCOMPARE(dumped(t.view), QString("1b 5b 3c 30 3b 32 3b 31 4d 1b 5b 3c 33 32 3b 34 3b 32 4d "
                                     "1b 5b 3c 30 3b 34 3b 32 6d"));
  }
  void onTheAlternateScreenTheWheelSendsArrowKeysAndTheViewStays() {
    Term t;
    QVERIFY(t.shown());
    QVERIFY(startLongDump(t.view, 18, "i=1; while [ $i -le 100 ]; do echo line $i; i=$((i+1)); done;"
                                      " printf '\\033[?1049h\\033[H'"));
    const TermScreen *s = t.view.screen();
    QVERIFY(s->altScreen());
    QVERIFY(s->scrollback() > 50);
    const QScrollBar *bar = t.view.verticalScrollBar();
    QCOMPARE(bar->maximum(), 0);  // nothing to look back at on this screen
    wheel(t.view, at(t.view, 3, 3), 1);   // three Up keys
    wheel(t.view, at(t.view, 3, 3), -1);  // three Down keys
    QCOMPARE(bar->maximum(), 0);
    QCOMPARE(bar->value(), 0);
    QVERIFY(commandKey(t.view, Qt::Key_A));  // everything there is to see: this screen
    QVERIFY(t.view.selectedText().startsWith("ready"));
    QCOMPARE(dumped(t.view), QString("1b 5b 41 1b 5b 41 1b 5b 41 1b 5b 42 1b 5b 42 1b 5b 42"));
  }
  void aFloodLeavesTheWindowAnswering() {
    qint64 pid = 0;
    {
      Term t;
      QVERIFY(t.shown());
      QVERIFY(t.run("/usr/bin/yes", {}));
      pid = t.view.pid();
      // a timer every 10 ms fires at least 20 times in every second of the flood, and never waits
      // more than 250 ms (one stall of a loaded machine is not the window stopping)
      QElapsedTimer clock;
      QList<qint64> ticks;
      QTimer tick;
      tick.setInterval(10);
      QObject::connect(&tick, &QTimer::timeout, &tick, [&] { ticks << clock.elapsed(); });
      QEventLoop loop;
      QTimer::singleShot(2000, &loop, &QEventLoop::quit);
      clock.start();
      tick.start();
      loop.exec();
      const qint64 end = clock.elapsed();
      qint64 last = 0, longest = 0;
      for (const qint64 ms : ticks) longest = std::max(longest, ms - std::exchange(last, ms));
      longest = std::max(longest, end - last);
      QVERIFY2(longest <= 250, qPrintable(QString("the event loop waited %1 ms").arg(longest)));
      for (qint64 from = 0; from + 1000 <= end; from += 50) {
        const auto fired = std::count_if(ticks.begin(), ticks.end(), [&](qint64 ms) { return ms >= from && ms < from + 1000; });
        QVERIFY2(fired >= 20, qPrintable(QString("%1 ticks in the second from %2 ms").arg(fired).arg(from)));
      }
      QCOMPARE(t.view.screen()->scrollback(), TermScreen::kScrollbackLimit);
      QVERIFY(t.view.running());
    }
    const int sent = ::kill(static_cast<pid_t>(pid), 0), why = errno;
    QCOMPARE(sent, -1);  // no such process: gone, and reaped
    QCOMPARE(why, ESRCH);
  }
  void newOutputUnderASelectionDropsIt() {
    Term t;
    QVERIFY(t.shown());
    t.view.screen()->feed("first\r\nsecond\r\nthird");
    drag(t.view, at(t.view, 0, 0), at(t.view, 1, 5));
    QCOMPARE(t.view.selectedText(), QString("first\nsecond"));
    t.view.screen()->feed("\x1b[10;1Hother");  // elsewhere
    QCOMPARE(t.view.selectedText(), QString("first\nsecond"));
    t.view.screen()->feed("\x1b[1;1Hnew");  // over it
    QCOMPARE(t.view.selectedText(), QString());
    // and a new size drops it
    drag(t.view, at(t.view, 2, 0), at(t.view, 2, 4));
    QCOMPARE(t.view.selectedText(), QString("third"));
    t.view.resize(t.view.width() + 5 * cellWidth(t.view), t.view.height());
    QVERIFY(QTest::qWaitFor([&] { return t.view.selectedText().isEmpty(); }));
  }
  void aSelectionInTheScrollbackStaysWhileOutputComesAndTheViewScrolls() {
    Term t;
    QVERIFY(t.shown(800, 24 * cellHeight(t.view) + 8));
    t.view.screen()->feed(numberedLines(1, 100));
    QScrollBar *bar = t.view.verticalScrollBar();
    bar->setValue(0);
    drag(t.view, at(t.view, 0, 0), at(t.view, 1, 5));
    QCOMPARE(t.view.selectedText(), QString("line 1\nline 2"));
    t.view.screen()->feed(numberedLines(101, 110));
    QCOMPARE(topRow(t.view), QString("line 1"));
    QCOMPARE(t.view.selectedText(), QString("line 1\nline 2"));
    wheel(t.view, t.view.viewport()->rect().center(), -1);
    QCOMPARE(topRow(t.view), QString("line 4"));
    QCOMPARE(t.view.selectedText(), QString("line 1\nline 2"));
  }
  void draggingPastTheTopScrollsBackAndSelectsOn() {
    Term t;
    QVERIFY(t.shown(800, 24 * cellHeight(t.view) + 8));
    t.view.screen()->feed(numberedLines(1, 100));  // row 0 is line 78, row 5 line 83
    const QScrollBar *bar = t.view.verticalScrollBar();
    const QPoint above(at(t.view, 0, 0).x(), -5);
    mouse(t.view, QEvent::MouseButtonPress, at(t.view, 5, 0));
    mouse(t.view, QEvent::MouseMove, above);
    QVERIFY(QTest::qWaitFor([&] { return bar->value() <= bar->maximum() - 5; }, 2000));
    mouse(t.view, QEvent::MouseButtonRelease, above);
    const int stopped = bar->value();
    QTest::qWait(150);
    QCOMPARE(bar->value(), stopped);  // the release stops it
    const QString text = t.view.selectedText();
    QVERIFY2(text.startsWith(topRow(t.view) + "\n"), qPrintable(text));
    QVERIFY2(text.endsWith("\nline 82\nl"), qPrintable(text));
  }
  void aWheelNotchIsThreeLinesWhateverPixelsMacOsGivesIt() {
    Term t;
    QVERIFY(t.shown(800, 24 * cellHeight(t.view) + 8));
    TermScreen *s = t.view.screen();
    const QScrollBar *bar = t.view.verticalScrollBar();
    s->feed(numberedLines(1, 100));
    const QPoint middle = t.view.viewport()->rect().center();
    wheel(t.view, middle, 1);  // looking back: three lines
    QCOMPARE(bar->value(), bar->maximum() - 3);
    wheel(t.view, middle, -1);
    QCOMPARE(bar->value(), bar->maximum());
    QSignalSpy sent(s, &TermScreen::reply);
    s->feed("\x1b[?1000h\x1b[?1006h");  // a program that asked for the mouse: one step
    wheel(t.view, at(t.view, 2, 4), 1);
    QCOMPARE(sentBy(sent), QByteArray("\x1b[<64;5;3M"));
    s->feed("\x1b[?1000l\x1b[?1049h");  // the alternate screen: three Up keys
    sent.clear();
    wheel(t.view, middle, 1);
    QCOMPARE(sentBy(sent), QByteArray("\x1b[A\x1b[A\x1b[A"));
  }
  void whatIsLeftOfATurnGoesWhenTheWheelTurnsBackOrDoesSomethingElse() {
    Term t;
    QVERIFY(t.shown(800, 24 * cellHeight(t.view) + 8));
    TermScreen *s = t.view.screen();
    const QScrollBar *bar = t.view.verticalScrollBar();
    s->feed(numberedLines(1, 100));
    const QPoint middle = t.view.viewport()->rect().center();
    const int ch = cellHeight(t.view), most = ch * 9 / 10, some = ch * 2 / 10, half = ch / 2;
    trackpad(t.view, middle, most);   // most of a line up
    trackpad(t.view, middle, -some);  // a little down: what was left of the way up goes
    trackpad(t.view, middle, half);   // half a line up: nothing yet
    QCOMPARE(bar->value(), bar->maximum());
    trackpad(t.view, middle, -half);
    trackpad(t.view, middle, most);   // most of a line up, looking back
    QSignalSpy sent(s, &TermScreen::reply);
    s->feed("\x1b[?1049h");
    trackpad(t.view, middle, half);   // then half a line as Up keys: nothing yet
    QCOMPARE(sentBy(sent), QByteArray());
  }
  void losingTheFocusEndsADrag() {
    Term t;
    QVERIFY(t.shown(800, 24 * cellHeight(t.view) + 8));
    QVERIFY(t.focused());
    t.view.screen()->feed(numberedLines(1, 100));
    const QScrollBar *bar = t.view.verticalScrollBar();
    mouse(t.view, QEvent::MouseButtonPress, at(t.view, 5, 0));
    mouse(t.view, QEvent::MouseMove, QPoint(at(t.view, 0, 0).x(), -5));  // past the top: it scrolls
    QVERIFY(QTest::qWaitFor([&] { return bar->value() <= bar->maximum() - 2; }, 2000));
    t.view.clearFocus();  // a dialog, another application: the release never comes
    QVERIFY(!t.view.hasFocus());
    const int stopped = bar->value();
    QTest::qWait(200);
    QCOMPARE(bar->value(), stopped);
    const QString selected = t.view.selectedText();
    mouse(t.view, QEvent::MouseMove, at(t.view, 10, 3));  // the drag is over
    QCOMPARE(t.view.selectedText(), selected);
  }
  void aWheelOrAClickOverThePaddingReachesNoParent() {
    struct Parent : QWidget {
      int wheels = 0, presses = 0;
      void wheelEvent(QWheelEvent *e) override {
        ++wheels;
        e->accept();
      }
      void mousePressEvent(QMouseEvent *e) override {
        ++presses;
        e->accept();
      }
    } parent;
    auto *view = new TerminalView(&parent);
    view->setGeometry(10, 10, 400, 300);
    parent.resize(420, 320);
    parent.show();
    QVERIFY(QTest::qWaitForWindowExposed(&parent));
    const QPointF padding(1, 1);  // the view's own, outside its viewport
    QWheelEvent wheel(padding, view->mapToGlobal(padding), QPoint(0, 20), QPoint(0, 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(view, &wheel);
    QMouseEvent press(QEvent::MouseButtonPress, padding, view->mapToGlobal(padding), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view, &press);
    QCOMPARE(parent.wheels, 0);
    QCOMPARE(parent.presses, 0);
  }
  void outputWhileLookingBackRepaintsNothingThatStays() {
    Term t;
    QVERIFY(t.shown(800, 24 * cellHeight(t.view) + 8));
    t.view.screen()->feed(numberedLines(1, 200));
    QScrollBar *bar = t.view.verticalScrollBar();
    bar->setValue(50);  // far back: no row of the screen in view
    QTest::qWait(100);
    const int before = t.view.paints();
    const QString top = topRow(t.view);
    t.view.screen()->feed(numberedLines(201, 210));
    QTest::qWait(100);
    QCOMPARE(topRow(t.view), top);
    QCOMPARE(t.view.paints(), before);
  }
  void goingOnOrOffTheAlternateScreenShowsAtOnceAndDropsTheSelection() {
    Term t;
    QVERIFY(t.shown(800, 24 * cellHeight(t.view) + 8));
    TermScreen *s = t.view.screen();
    const QScrollBar *bar = t.view.verticalScrollBar();
    s->feed(numberedLines(1, 100));
    wheel(t.view, t.view.viewport()->rect().center(), 1);
    drag(t.view, at(t.view, 0, 0), at(t.view, 0, 3));  // in the scrollback
    QCOMPARE(t.view.selectedText(), QString("line"));
    s->feed("\x1b[?1047h");  // on: nothing to look back at, and that text is not shown
    QCOMPARE(bar->maximum(), 0);
    QCOMPARE(bar->value(), 0);
    QCOMPARE(t.view.selectedText(), QString());
    s->feed("\x1b[Halt");
    drag(t.view, at(t.view, 0, 0), at(t.view, 0, 2));
    QCOMPARE(t.view.selectedText(), QString("alt"));
    s->feed("\x1b[?1047l");  // off: the scrollback again, at the bottom
    QCOMPARE(bar->maximum(), s->scrollback());
    QCOMPARE(bar->value(), bar->maximum());
    QCOMPARE(t.view.selectedText(), QString());
    s->feed("\x1b[?1049h");
    QCOMPARE(bar->maximum(), 0);
    s->feed("\x1b[?1049l");
    QCOMPARE(bar->maximum(), s->scrollback());
    s->feed("\x1b[?47h");  // libvterm 0.3.3 has no mode 47: the screen stays, and so does the view
    QVERIFY(!s->altScreen());
    QCOMPARE(bar->maximum(), s->scrollback());
  }
  void whileLookingBackTheWheelIsTheViews() {
    Term t;
    QVERIFY(t.shown(800, 24 * cellHeight(t.view) + 8));
    TermScreen *s = t.view.screen();
    QScrollBar *bar = t.view.verticalScrollBar();
    s->feed(numberedLines(1, 100));
    s->feed("\x1b[?1000h\x1b[?1006h");
    bar->setValue(bar->maximum() - 6);  // looking back, by the scroll bar
    QSignalSpy sent(s, &TermScreen::reply);
    wheel(t.view, at(t.view, 2, 4), -1);  // down: three lines nearer the bottom
    QCOMPARE(bar->value(), bar->maximum() - 3);
    wheel(t.view, at(t.view, 2, 4), -1);
    QCOMPARE(bar->value(), bar->maximum());
    QCOMPARE(sentBy(sent), QByteArray());
    wheel(t.view, at(t.view, 2, 4), 1);  // at the bottom it is the program's
    QCOMPARE(sentBy(sent), QByteArray("\x1b[<64;5;3M"));
    QCOMPARE(bar->value(), bar->maximum());
  }
  void atTheLimitTheLinesInViewAndTheSelectionStayUntilTheyLeave() {
    Term t;
    QVERIFY(t.shown(800, 24 * cellHeight(t.view) + 8));
    TermScreen *s = t.view.screen();
    QScrollBar *bar = t.view.verticalScrollBar();
    const int limit = TermScreen::kScrollbackLimit;
    s->feed(numberedLines(1, limit + 100));  // line 78 is the oldest left
    QCOMPARE(s->scrollback(), limit);
    bar->setValue(10);
    QCOMPARE(topRow(t.view), QString("line 88"));
    drag(t.view, at(t.view, 0, 0), at(t.view, 1, 6));
    QCOMPARE(t.view.selectedText(), QString("line 88\nline 89"));
    s->feed(numberedLines(limit + 101, limit + 105));  // five more leave: 88 is still there
    QCOMPARE(bar->value(), 5);
    QCOMPARE(topRow(t.view), QString("line 88"));
    QCOMPARE(t.view.selectedText(), QString("line 88\nline 89"));
    s->feed(numberedLines(limit + 106, limit + 115));  // and now it has left
    QCOMPARE(bar->value(), 0);
    QCOMPARE(topRow(t.view), QString("line 93"));
    QCOMPARE(t.view.selectedText(), QString());
  }
  void clearingTheScrollbackWhileLookingBackComesBackToTheBottom() {
    Term t;
    QVERIFY(t.shown(800, 24 * cellHeight(t.view) + 8));
    TermScreen *s = t.view.screen();
    const QScrollBar *bar = t.view.verticalScrollBar();
    s->feed(numberedLines(1, 100));
    wheel(t.view, t.view.viewport()->rect().center(), 1);
    QCOMPARE(bar->value(), bar->maximum() - 3);
    s->feed("\x1b[3J");
    QCOMPARE(s->scrollback(), 0);
    QCOMPARE(bar->maximum(), 0);
    QCOMPARE(bar->value(), 0);
    QCOMPARE(topRow(t.view), s->text(0));
  }
  void commandASelectsEverythingAndOutputDoesNotEndIt() {
    KeptClipboard kept;
    Term t;
    QVERIFY(t.shown());
    TermScreen *s = t.view.screen();
    s->feed("old\r\n");
    QVERIFY(commandKey(t.view, Qt::Key_A));
    s->feed("new\r\n");  // output, over rows that are selected
    QVERIFY(commandKey(t.view, Qt::Key_C));
    QCOMPARE(QGuiApplication::clipboard()->text(), QString("old\nnew"));
    const Picture pic(t.view);  // drawn as everything selected
    QCOMPARE(pic.colour(pic.cells(10, 30).topLeft()), foreground);
    mouse(t.view, QEvent::MouseButtonPress, at(t.view, 0, 0));  // a press ends it
    mouse(t.view, QEvent::MouseButtonRelease, at(t.view, 0, 0));
    QCOMPARE(t.view.selectedText(), QString());
    QVERIFY(commandKey(t.view, Qt::Key_A));
    const QPointF p(at(t.view, 3, 3));
    QMouseEvent right(QEvent::MouseButtonPress, p, t.view.viewport()->mapToGlobal(p), Qt::RightButton,
                      Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(t.view.viewport(), &right);  // any press
    QCOMPARE(t.view.selectedText(), QString());
    QMouseEvent rightUp(QEvent::MouseButtonRelease, p, t.view.viewport()->mapToGlobal(p), Qt::RightButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(t.view.viewport(), &rightUp);
    QVERIFY(commandKey(t.view, Qt::Key_A));
    s->feed("\x1b[?1049h");  // and so does the alternate screen
    QCOMPARE(t.view.selectedText(), QString());
  }
};

QTEST_MAIN(TestTerminalView)
#include "test_terminal_view.moc"
