#include "term_selection.hpp"

#include <algorithm>

#include "term_screen.hpp"

namespace {

TermCell cellAt(const TermScreen &s, TermPoint p) { return s.cell(TermSelection::rowOf(s, p.line), p.col); }

// never written, or a space. The second half of a wide character is not: it goes with the first
bool whitespace(const TermCell &c) { return c.width != 0 && c.text.trimmed().isEmpty(); }

// the cell before p in reading order, back onto the row above only when that row wrapped onto p's
// row; false when there is none
bool stepBack(const TermScreen &s, TermPoint &p) {
  if (p.col > 0) {
    --p.col;
    return true;
  }
  if (p.line <= TermSelection::topLine(s) || !s.wrapped(TermSelection::rowOf(s, p.line) - 1)) return false;
  --p.line;
  p.col = s.cols() - 1;
  return true;
}

// the cell after p, on to the next row only when p's row wrapped onto it
bool stepOn(const TermScreen &s, TermPoint &p) {
  if (p.col < s.cols() - 1) {
    ++p.col;
    return true;
  }
  if (p.line >= TermSelection::bottomLine(s) || !s.wrapped(TermSelection::rowOf(s, p.line))) return false;
  ++p.line;
  p.col = 0;
  return true;
}

// the ends of the word at p: the cells around it that are whitespace as it is, or are not
TermPoint wordStart(const TermScreen &s, TermPoint p) {
  const bool space = whitespace(cellAt(s, p));
  for (TermPoint q = p; stepBack(s, q) && whitespace(cellAt(s, q)) == space;) p = q;
  return p;
}

TermPoint wordEnd(const TermScreen &s, TermPoint p) {
  const bool space = whitespace(cellAt(s, p));
  for (TermPoint q = p; stepOn(s, q) && whitespace(cellAt(s, q)) == space;) p = q;
  return p;
}

TermPoint clamped(const TermScreen &s, TermPoint p) {
  return {std::clamp(p.line, TermSelection::topLine(s), TermSelection::bottomLine(s)),
          std::clamp(p.col, 0, s.cols() - 1)};
}

}  // namespace

qint64 TermSelection::topLine(const TermScreen &s) {
  return s.linesDropped() + (s.altScreen() ? s.scrollback() : 0);
}

qint64 TermSelection::bottomLine(const TermScreen &s) {
  return s.linesDropped() + s.scrollback() + s.rows() - 1;
}

int TermSelection::rowOf(const TermScreen &s, qint64 line) {
  return static_cast<int>(line - s.linesDropped() - s.scrollback());
}

void TermSelection::start(const TermScreen &s, TermPoint p, Unit unit) {
  unit_ = unit;
  anchor_ = head_ = clamped(s, p);
  started_ = true;
  grown_ = everything_ = false;
  spanUnits(s);
}

void TermSelection::extend(const TermScreen &s, TermPoint p) {
  if (!started_) return;
  head_ = clamped(s, p);
  grown_ = grown_ || head_ != anchor_;
  spanUnits(s);
}

void TermSelection::all() {
  started_ = grown_ = false;
  everything_ = true;
}

void TermSelection::clear() { started_ = grown_ = everything_ = false; }
bool TermSelection::empty() const { return !everything_ && (!started_ || (unit_ == Unit::Cell && !grown_)); }
bool TermSelection::everything() const { return everything_; }
TermPoint TermSelection::first() const { return first_; }
TermPoint TermSelection::last() const { return last_; }

bool TermSelection::contains(TermPoint p) const {
  return everything_ || (!empty() && first_ <= p && p <= last_);
}

// first_ and last_: the press and the pointer in reading order, widened to whole words or rows, and
// to all of a wide character the first cell is half of
void TermSelection::spanUnits(const TermScreen &s) {
  first_ = std::min(anchor_, head_);
  last_ = std::max(anchor_, head_);
  if (unit_ == Unit::Word) {
    first_ = wordStart(s, first_);
    last_ = wordEnd(s, last_);
  } else if (unit_ == Unit::Line) {
    first_.col = 0;
    last_.col = s.cols() - 1;
  }
  if (first_.col > 0 && cellAt(s, first_).width == 0) --first_.col;
}

QString TermSelection::text(const TermScreen &s) const {
  if (empty()) return {};
  if (!everything_) return textBetween(s, first_, last_);
  QString all = textBetween(s, {topLine(s), 0}, {bottomLine(s), s.cols() - 1});
  while (all.endsWith('\n')) all.chop(1);  // the empty rows below the last text
  return all;
}

QString TermSelection::textBetween(const TermScreen &s, TermPoint first, TermPoint last) {
  QString out;
  for (qint64 line = first.line; line <= last.line; ++line) {
    const int row = rowOf(s, line);
    const int from = line == first.line ? first.col : 0;
    const int to = line == last.line ? last.col : s.cols() - 1;
    // a row that wrapped goes on into the next with nothing between. From its end only the cells
    // never written are dropped (a wide character that did not fit leaves one); a space there is text
    const bool wraps = line < last.line && s.wrapped(row);
    QString part;
    qsizetype kept = 0;  // part's length up to the last cell kept
    for (int col = from; col <= to; ++col) {
      const TermCell c = s.cell(row, col);
      if (c.width == 0) continue;  // the second half of a wide character
      part += c.text.isEmpty() ? QStringLiteral(" ") : c.text;
      if (wraps ? !c.text.isEmpty() : !whitespace(c)) kept = part.size();
    }
    out += part.left(kept);
    if (line < last.line && !wraps) out += '\n';
  }
  return out;
}
