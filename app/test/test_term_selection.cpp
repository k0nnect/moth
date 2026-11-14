#include <QtTest>

#include "term_screen.hpp"
#include "term_selection.hpp"

namespace {

using Unit = TermSelection::Unit;

// the text selected by a drag from one cell to another
QString dragged(const TermScreen &s, TermPoint from, TermPoint to) {
  TermSelection sel;
  sel.start(s, from, Unit::Cell);
  sel.extend(s, to);
  return sel.text(s);
}

}  // namespace

class TestTermSelection : public QObject {
  Q_OBJECT
private slots:
  void cellsInReadingOrder() {
    TermScreen s(5, 20);
    s.feed("first line\r\nsecond line\r\nthird");
    TermSelection sel;
    sel.start(s, {1, 3}, Unit::Cell);
    QVERIFY(sel.empty());  // a press alone selects nothing
    sel.extend(s, {1, 3});
    QVERIFY(sel.empty());
    sel.extend(s, {2, 2});
    QCOMPARE(sel.text(s), QString("ond line\nthi"));
    sel.extend(s, {0, 6});  // back past where it started, which stays in it
    QCOMPARE(sel.first(), (TermPoint{0, 6}));
    QCOMPARE(sel.last(), (TermPoint{1, 3}));
    QCOMPARE(sel.text(s), QString("line\nseco"));
    QVERIFY(sel.contains({0, 19}));
    QVERIFY(sel.contains({1, 0}));
    QVERIFY(!sel.contains({0, 5}));
    QVERIFY(!sel.contains({1, 4}));
    sel.extend(s, {1, 3});  // and back to it: that cell
    QCOMPARE(sel.text(s), QString("o"));
    sel.clear();
    QVERIFY(sel.empty());
    QVERIFY(!sel.contains({1, 3}));
    QCOMPARE(sel.text(s), QString());
  }
  void trailingBlanksDroppedWrappedRowsJoinedWideCharactersOnce() {
    TermScreen s(8, 10);
    s.feed("ab   \r\n"
           "aaaaaaaaaabb\r\n"                                    // wraps
           "xxxxx yyy z\r\n" +                                   // wraps after a space
           QString::fromUtf8("aaaaaaaaa日\r\n").toUtf8());        // 日 does not fit: wraps
    TermSelection sel;
    sel.all(s);
    QCOMPARE(sel.text(s), QString::fromUtf8("ab\naaaaaaaaaabb\nxxxxx yyy z\naaaaaaaaa日\n"));
    QCOMPARE(dragged(s, {6, 1}, {6, 5}), QString::fromUtf8("日"));  // from its second half
    QCOMPARE(dragged(s, {5, 8}, {6, 0}), QString::fromUtf8("a日"));
    QCOMPARE(dragged(s, {0, 0}, {0, 9}), QString("ab"));
    QCOMPARE(dragged(s, {3, 8}, {4, 0}), QString("y z"));
  }
  void aWordIsARunOfCharactersThatAreNotWhitespace() {
    TermScreen s(4, 20);
    s.feed("hello world  x/y.z\r\n");
    TermSelection sel;
    sel.start(s, {0, 8}, Unit::Word);
    QCOMPARE(sel.text(s), QString("world"));
    sel.start(s, {0, 0}, Unit::Word);
    QCOMPARE(sel.text(s), QString("hello"));
    sel.start(s, {0, 15}, Unit::Word);
    QCOMPARE(sel.text(s), QString("x/y.z"));
    sel.extend(s, {0, 2});  // a drag after a double click goes by words
    QCOMPARE(sel.text(s), QString("hello world  x/y.z"));
  }
  void aWordGoesOnAcrossARowThatWrapped() {
    TermScreen s(4, 10);
    s.feed("ab https://example.com/x ok");
    TermSelection sel;
    sel.start(s, {1, 4}, Unit::Word);
    QCOMPARE(sel.text(s), QString("https://example.com/x"));
    QCOMPARE(sel.first(), (TermPoint{0, 3}));
    QCOMPARE(sel.last(), (TermPoint{2, 3}));
  }
  void aLineIsTheWholeRow() {
    TermScreen s(4, 10);
    s.feed("one two\r\nthree");
    TermSelection sel;
    sel.start(s, {0, 2}, Unit::Line);
    QCOMPARE(sel.text(s), QString("one two"));
    sel.extend(s, {1, 0});
    QCOMPARE(sel.text(s), QString("one two\nthree"));
    QCOMPARE(sel.last(), (TermPoint{1, 9}));
  }
  void allIsTheScrollbackAndTheScreenButOnTheAlternateScreenItself() {
    TermScreen s(3, 10);
    s.feed("1\r\n2\r\n3\r\n4\r\n5");  // 1 and 2 scroll off
    TermSelection sel;
    sel.all(s);
    QCOMPARE(sel.first(), (TermPoint{0, 0}));
    QCOMPARE(sel.last(), (TermPoint{4, 9}));
    QCOMPARE(sel.text(s), QString("1\n2\n3\n4\n5"));
    s.feed("\x1b[?1049h\x1b[Halt");
    sel.all(s);
    QCOMPARE(sel.first(), (TermPoint{2, 0}));
    QCOMPARE(sel.text(s), QString("alt\n\n"));
  }
  void linesAreCountedFromTheOldestSoASelectionStaysOnItsText() {
    TermScreen s(3, 10);
    s.feed("1\r\n2\r\n3");
    TermSelection sel;
    sel.start(s, {1, 0}, Unit::Cell);
    sel.extend(s, {2, 0});
    QCOMPARE(sel.text(s), QString("2\n3"));
    s.feed("\r\n4\r\n5");  // 1 and 2 scroll off
    QCOMPARE(s.scrollback(), 2);
    QCOMPARE(sel.text(s), QString("2\n3"));
  }
};

QTEST_MAIN(TestTermSelection)
#include "test_term_selection.moc"
