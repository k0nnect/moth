#include <QSignalSpy>
#include <QtTest>

#include "term_screen.hpp"

namespace {

// a screen and everything it asked to be sent back
struct Scr {
  TermScreen s;
  QByteArray sent;
  explicit Scr(int rows = 24, int cols = 80) : s(rows, cols) {
    QObject::connect(&s, &TermScreen::reply, &s, [this](const QByteArray &b) { sent += b; });
  }
  void feed(const QByteArray &b) { s.feed(b); }
  QByteArray take() { return std::exchange(sent, QByteArray()); }
  // what a key sends
  QByteArray key(int k, Qt::KeyboardModifiers m = Qt::NoModifier, const QString &text = "") {
    sent.clear();
    s.key(k, m, text);
    return take();
  }
};

}  // namespace

class TestTermScreen : public QObject {
  Q_OBJECT
private slots:
  void textAndColour() {
    Scr t;
    t.feed("hi\r\n\x1b[31mred");
    QCOMPARE(t.s.text(0), QString("hi"));
    const TermCell c = t.s.cell(1, 0);
    QCOMPARE(c.text, QString("r"));
    QVERIFY(c.fg.isValid());
    QVERIFY(c.fg.red() > 150 && c.fg.green() < 60 && c.fg.blue() < 60);
    QVERIFY(!c.bg.isValid());
    QCOMPARE(t.s.cursor(), QPoint(3, 1));
    QVERIFY(!t.s.cell(0, 0).fg.isValid());  // the default is not a colour
  }
  void indexedColours() {
    Scr t;
    t.feed("\x1b[38;5;196ma\x1b[48;5;21mb\x1b[38;5;240mc");
    QCOMPARE(t.s.cell(0, 0).fg, QColor(255, 0, 0));
    QCOMPARE(t.s.cell(0, 1).bg, QColor(0, 0, 255));
    QCOMPARE(t.s.cell(0, 2).fg, QColor(88, 88, 88));
  }
  void attributes() {
    Scr t;
    t.feed("\x1b[38;2;10;20;30;48;2;200;210;220mA\x1b[0m\x1b[1mB\x1b[0m\x1b[3mC\x1b[0m\x1b[4mD\x1b[0m\x1b[7mE");
    QCOMPARE(t.s.cell(0, 0).fg, QColor(10, 20, 30));
    QCOMPARE(t.s.cell(0, 0).bg, QColor(200, 210, 220));
    QVERIFY(t.s.cell(0, 1).bold && !t.s.cell(0, 1).italic);
    QVERIFY(t.s.cell(0, 2).italic && !t.s.cell(0, 2).bold);
    QVERIFY(t.s.cell(0, 3).underline);
    QVERIFY(t.s.cell(0, 4).reverse);
    QVERIFY(!t.s.cell(0, 0).bold && !t.s.cell(0, 0).underline && !t.s.cell(0, 0).reverse);
  }
  void wideCharacters() {
    Scr t;
    t.feed(QString::fromUtf8("日本").toUtf8());
    QCOMPARE(t.s.cell(0, 0).text, QString::fromUtf8("日"));
    QCOMPARE(t.s.cell(0, 0).width, 2);
    QCOMPARE(t.s.cell(0, 1).width, 0);
    QCOMPARE(t.s.cell(0, 1).text, QString());
    QCOMPARE(t.s.cell(0, 2).text, QString::fromUtf8("本"));
    QCOMPARE(t.s.cell(0, 2).width, 2);
    QCOMPARE(t.s.cell(0, 3).width, 0);
    QCOMPARE(t.s.text(0), QString::fromUtf8("日本"));
    QCOMPARE(t.s.cursor(), QPoint(4, 0));
  }
  void emojiWithVariationSelector() {
    Scr t;
    const QString thumbs = QString::fromUcs4(U"\U0001F44D\uFE0F");  // an emoji and its selector
    t.feed((thumbs + "x").toUtf8());
    QCOMPARE(t.s.cell(0, 0).width, 2);
    QCOMPARE(t.s.cell(0, 0).text, thumbs);
    QCOMPARE(t.s.cell(0, 1).width, 0);
    QCOMPARE(t.s.cell(0, 2).text, QString("x"));
    QCOMPARE(t.s.cursor(), QPoint(3, 0));
  }
  void scrollback() {
    Scr t;
    QSignalSpy scrolled(&t.s, &TermScreen::scrolled);
    QByteArray all;
    for (int i = 1; i <= 100; ++i) all += "line " + QByteArray::number(i) + (i < 100 ? "\r\n" : "");
    t.feed(all);
    QCOMPARE(t.s.scrollback(), 76);
    QVERIFY(scrolled.count() >= 1);
    QCOMPARE(t.s.text(-1), QString("line 76"));
    QCOMPARE(t.s.text(-76), QString("line 1"));
    QCOMPARE(t.s.text(0), QString("line 77"));
    QCOMPARE(t.s.text(23), QString("line 100"));
    QCOMPARE(t.s.text(-77), QString());
    QCOMPARE(t.s.cell(-77, 0).text, QString());
    QCOMPARE(t.s.cell(24, 0).text, QString());
    QCOMPARE(t.s.cell(0, 80).text, QString());
  }
  void scrollbackIsCapped() {
    Scr t;
    QByteArray all;
    for (int i = 1; i <= 20000; ++i) all += "n" + QByteArray::number(i) + "\r\n";
    t.feed(all);
    QCOMPARE(t.s.scrollback(), TermScreen::kScrollbackLimit);
    QCOMPARE(t.s.text(-1), QString("n19977"));  // 20000 lines + the empty one under the last: 24 rows
  }
  void alternateScreen() {
    Scr t;
    QByteArray all;
    for (int i = 1; i <= 30; ++i) all += "old " + QByteArray::number(i) + "\r\n";
    t.feed(all);
    const int before = t.s.scrollback();
    const QString top = t.s.text(0), bottom = t.s.text(22);
    QVERIFY(!t.s.altScreen());
    t.feed("\x1b[?1049h");
    QVERIFY(t.s.altScreen());
    QCOMPARE(t.s.text(0), QString());
    QByteArray alt;
    for (int i = 1; i <= 60; ++i) alt += "alt " + QByteArray::number(i) + "\r\n";
    t.feed(alt);
    QCOMPARE(t.s.scrollback(), before);
    t.feed("\x1b[?1049l");
    QVERIFY(!t.s.altScreen());
    QCOMPARE(t.s.scrollback(), before);
    QCOMPARE(t.s.text(0), top);
    QCOMPARE(t.s.text(22), bottom);
  }
  void titleAndBell() {
    Scr t;
    QSignalSpy changed(&t.s, &TermScreen::titleChanged), bell(&t.s, &TermScreen::bell);
    t.feed("\x1b]2;moth\x07");
    QCOMPARE(t.s.title(), QString("moth"));
    QCOMPARE(changed.count(), 1);
    t.feed("\x07");
    QCOMPARE(bell.count(), 1);
  }
  void cursorVisibility() {
    Scr t;
    QVERIFY(t.s.cursorVisible());
    t.feed("\x1b[?25l");
    QVERIFY(!t.s.cursorVisible());
    t.feed("\x1b[?25h");
    QVERIFY(t.s.cursorVisible());
  }
  void damageIsReported() {
    Scr t;
    QSignalSpy d(&t.s, &TermScreen::damaged);
    t.feed("\x1b[5;1Hx");
    QVERIFY(d.count() >= 1);
    bool row4 = false;
    for (const auto &a : d) row4 |= a[0].toInt() <= 4 && a[1].toInt() >= 4;
    QVERIFY(row4);
  }
  void keys() {
    Scr t;
    QCOMPARE(t.key(Qt::Key_Up), QByteArray("\x1b[A"));
    t.feed("\x1b[?1h");
    QCOMPARE(t.key(Qt::Key_Up), QByteArray("\x1bOA"));
    t.feed("\x1b[?1l");
    QCOMPARE(t.key(Qt::Key_Down), QByteArray("\x1b[B"));
    QCOMPARE(t.key(Qt::Key_Right), QByteArray("\x1b[C"));
    QCOMPARE(t.key(Qt::Key_Left), QByteArray("\x1b[D"));
    QCOMPARE(t.key(Qt::Key_Return), QByteArray("\r"));
    QCOMPARE(t.key(Qt::Key_Enter), QByteArray("\r"));
    QCOMPARE(t.key(Qt::Key_Tab), QByteArray("\t"));
    QCOMPARE(t.key(Qt::Key_Backtab, Qt::ShiftModifier), QByteArray("\x1b[Z"));
    QCOMPARE(t.key(Qt::Key_Backspace), QByteArray("\x7f"));
    QCOMPARE(t.key(Qt::Key_Escape), QByteArray("\x1b"));
    QCOMPARE(t.key(Qt::Key_Home), QByteArray("\x1b[H"));
    QCOMPARE(t.key(Qt::Key_End), QByteArray("\x1b[F"));
    QCOMPARE(t.key(Qt::Key_PageUp), QByteArray("\x1b[5~"));
    QCOMPARE(t.key(Qt::Key_PageDown), QByteArray("\x1b[6~"));
    QCOMPARE(t.key(Qt::Key_Insert), QByteArray("\x1b[2~"));
    QCOMPARE(t.key(Qt::Key_Delete), QByteArray("\x1b[3~"));
    QCOMPARE(t.key(Qt::Key_F1), QByteArray("\x1bOP"));
    QCOMPARE(t.key(Qt::Key_F5), QByteArray("\x1b[15~"));
    QCOMPARE(t.key(Qt::Key_F12), QByteArray("\x1b[24~"));
    QCOMPARE(t.key(Qt::Key_Up, Qt::ShiftModifier), QByteArray("\x1b[1;2A"));
    QCOMPARE(t.key(Qt::Key_Up, Qt::ControlModifier), QByteArray("\x1b[1;5A"));
  }
  void textKeys() {
    Scr t;
    QCOMPARE(t.key(Qt::Key_A, Qt::NoModifier, "a"), QByteArray("a"));
    QCOMPARE(t.key(Qt::Key_A, Qt::ShiftModifier, "A"), QByteArray("A"));
    QCOMPARE(t.key(Qt::Key_C, Qt::ControlModifier, "\x03"), QByteArray("\x03"));
    QCOMPARE(t.key(Qt::Key_C, Qt::ControlModifier, ""), QByteArray("\x03"));
    QCOMPARE(t.key(Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier, ""), QByteArray("\x04"));
    QCOMPARE(t.key(Qt::Key_B, Qt::AltModifier, "b"), QByteArray("\x1b" "b"));
    QCOMPARE(t.key(Qt::Key_Eacute, Qt::NoModifier, QString::fromUtf8("é")), QByteArray("\xc3\xa9"));
    QCOMPARE(t.key(Qt::Key_Space, Qt::NoModifier, " "), QByteArray(" "));
    QCOMPARE(t.key(Qt::Key_A, Qt::NoModifier, ""), QByteArray("a"));  // no text: the key itself
    QCOMPARE(t.key(Qt::Key_Shift, Qt::ShiftModifier, ""), QByteArray());
  }
  void optionLeft() {
    Scr t;
    // xterm's encoding of Alt+Left. (macOS Terminal sends ESC b; the brief expected that)
    QCOMPARE(t.key(Qt::Key_Left, Qt::AltModifier), QByteArray("\x1b[1;3D"));
  }
  void paste() {
    Scr t;
    t.s.paste("a\nb");
    QCOMPARE(t.take(), QByteArray("a\rb"));
    t.feed("\x1b[?2004h");
    t.s.paste("a\nb");
    QCOMPARE(t.take(), QByteArray("\x1b[200~a\rb\x1b[201~"));
    t.s.paste(QString::fromUtf8("é\r\nx"));
    QCOMPARE(t.take(), QByteArray("\x1b[200~\xc3\xa9\rx\x1b[201~"));
  }
  void mouse() {
    Scr t;
    QVERIFY(!t.s.mouseWanted());
    t.s.mouse(2, 5, Qt::LeftButton, true, Qt::NoModifier);
    QCOMPARE(t.take(), QByteArray());
    t.s.wheel(2, 5, 1);
    QCOMPARE(t.take(), QByteArray());
    t.feed("\x1b[?1000h\x1b[?1006h");
    QVERIFY(t.s.mouseWanted());
    t.s.mouse(2, 5, Qt::LeftButton, true, Qt::NoModifier);
    QCOMPARE(t.take(), QByteArray("\x1b[<0;6;3M"));
    t.s.mouse(2, 5, Qt::LeftButton, false, Qt::NoModifier);
    QCOMPARE(t.take(), QByteArray("\x1b[<0;6;3m"));
    t.s.mouse(0, 0, Qt::RightButton, true, Qt::ShiftModifier);
    QCOMPARE(t.take(), QByteArray("\x1b[<6;1;1M"));
    t.s.wheel(1, 1, 2);
    QCOMPARE(t.take(), QByteArray("\x1b[<64;2;2M\x1b[<64;2;2M"));
    t.s.wheel(1, 1, -1);
    QCOMPARE(t.take(), QByteArray("\x1b[<65;2;2M"));
    t.feed("\x1b[?1000l");
    QVERIFY(!t.s.mouseWanted());
  }
  void deviceStatusReport() {
    Scr t;
    t.feed("ab\r\nxyz\x1b[6n");
    QCOMPARE(t.take(), QByteArray("\x1b[2;4R"));
  }
  void resizeKeepsTheBottomRows() {
    Scr t;
    QByteArray all;
    for (int i = 1; i <= 24; ++i) all += "row " + QByteArray::number(i) + (i < 24 ? "\r\n" : "");
    t.feed(all);
    t.s.resize(10, 40);
    QCOMPARE(t.s.rows(), 10);
    QCOMPARE(t.s.cols(), 40);
    QCOMPARE(t.s.text(9), QString("row 24"));
    QCOMPARE(t.s.text(8), QString("row 23"));
    QCOMPARE(t.s.text(0), QString("row 15"));
    QCOMPARE(t.s.text(-1), QString("row 14"));
    QCOMPARE(t.s.cursor().y(), 9);
    t.s.resize(24, 80);
    QCOMPARE(t.s.text(23), QString("row 24"));
    QCOMPARE(t.s.text(0), QString("row 1"));  // the lines that scrolled off come back
  }
};

QTEST_MAIN(TestTermScreen)
#include "test_term_screen.moc"
