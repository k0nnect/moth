#pragma once

#include <QString>
#include <compare>

class TermScreen;

// a cell of a terminal's text: its line, counted from the oldest line of the scrollback (so that it
// stays on its text while lines scroll off the screen), and its column
struct TermPoint {
  int line = 0, col = 0;
  auto operator<=>(const TermPoint &) const = default;
};

// what is selected of a terminal's text: every cell from one to another in reading order, across
// lines (as Terminal.app selects, not a rectangle). It grows from a press: by cells as the pointer
// is dragged, by words after a double click (a run of characters that are not whitespace, or one of
// whitespace, going on across a row that wrapped), by whole rows after a triple click.
//
// The screen is passed in, not kept. A selection is only as good as the lines under it: its owner
// drops it when they change.
class TermSelection {
public:
  enum class Unit { Cell, Word, Line };

  // the lines there are to select: the scrollback and the screen, or on the alternate screen that
  // screen alone (the scrollback is not its program's)
  static int topLine(const TermScreen &s);
  static int bottomLine(const TermScreen &s);

  // a press at p. By cells nothing is selected until extend() reaches another cell; by words or
  // rows the one at p is selected at once
  void start(const TermScreen &s, TermPoint p, Unit unit);
  // the pointer is now at p: from the press to p, by the unit the press began
  void extend(const TermScreen &s, TermPoint p);
  void all(const TermScreen &s);
  void clear();
  bool empty() const;
  TermPoint first() const;  // the first cell selected
  TermPoint last() const;   // the last, itself selected
  bool contains(TermPoint p) const;
  // the cells' text: each row's from the first selected to the last, trailing blanks dropped, rows
  // joined by a new line except where a row wrapped onto the next; a wide character once
  QString text(const TermScreen &s) const;

private:
  void spanUnits(const TermScreen &s);

  Unit unit_ = Unit::Cell;
  TermPoint anchor_, head_;  // where the press was, where the pointer is
  TermPoint first_, last_;   // anchor_ to head_ in order, widened to whole words or rows
  bool started_ = false;
  bool grown_ = false;       // by cells: the pointer has left the cell it was pressed on
};
