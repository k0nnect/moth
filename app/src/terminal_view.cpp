#include "terminal_view.hpp"

#include <QClipboard>
#include <QCoreApplication>
#include <QFontMetrics>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScrollBar>
#include <QStyleHints>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <utility>

#include "pty.hpp"
#include "term_screen.hpp"

namespace {

const QColor kBackground(0x16, 0x16, 0x16);
const QColor kForeground(0xd8, 0xd8, 0xd8);
constexpr int kDefaultRows = 24, kDefaultCols = 80;  // until the view is first sized
constexpr int kMinRows = 2, kMinCols = 10;
constexpr int kPadding = 4;                          // around the cells, outside the viewport
constexpr int kRepaintMs = 16;
constexpr int kWheelNotch = 120, kLinesPerNotch = 3;
constexpr int kDragScrollMs = 50;

// fed to the screen before a new program: off the alternate screen and mouse reports off (libvterm's
// reset leaves both as they were), a full reset, the scrollback cleared, the title emptied
const QByteArray kFreshTerminal =
    "\x1b[?1049l\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b" "c\x1b[3J\x1b]2;\x1b\\";

// a key's modifiers in the terminal's terms. On macOS Qt reports the Control key as Meta and
// Command as Control (unless the application asks it not to swap them); Option is Alt either way.
struct KeyMods {
  bool command = false;          // a key with Command is never the program's
  bool commandAlone = false;     // Command and no other modifier
  Qt::KeyboardModifiers terminal;  // Control, Alt (Option) and Shift as TermScreen::key reads them
};

KeyMods translate(Qt::KeyboardModifiers qt) {
  KeyMods m;
  m.terminal = qt & (Qt::ShiftModifier | Qt::AltModifier | Qt::KeypadModifier);
#ifdef Q_OS_MACOS
  const bool swapped = !QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
  const Qt::KeyboardModifiers command(swapped ? Qt::ControlModifier : Qt::MetaModifier);
  m.command = qt & command;
  m.commandAlone = (qt & ~Qt::KeypadModifier) == command;
  if (qt & (swapped ? Qt::MetaModifier : Qt::ControlModifier)) m.terminal |= Qt::ControlModifier;
#else
  m.terminal |= qt & Qt::ControlModifier;
#endif
  return m;
}

// Cmd+C, Cmd+V and Cmd+A, which the view keeps for itself (copy, paste, select all); they send
// nothing. Any other chord with Command is the application's.
bool viewCommand(int key, const KeyMods &mods) {
  return mods.commandAlone && (key == Qt::Key_C || key == Qt::Key_V || key == Qt::Key_A);
}

// a key that only changes the others: pressing it alone sends nothing
bool modifierKey(int key) {
  return key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Meta || key == Qt::Key_Alt ||
         key == Qt::Key_AltGr || key == Qt::Key_CapsLock;
}

// text a key press sends as it is: no control characters
bool plainText(const QString &text) {
  return !text.isEmpty() &&
         std::none_of(text.begin(), text.end(), [](QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f; });
}

// the character a key makes with Shift alone. Qt's code for a key that makes a character is that
// character (a letter's in upper case); codes from Key_Escape on are keys that make none
QString keyCharacter(int key, Qt::KeyboardModifiers mods) {
  if (key < Qt::Key_Space || key >= Qt::Key_Escape) return {};
  const char32_t c = static_cast<char32_t>(key);
  const QString s = QString::fromUcs4(&c, 1);
  return (mods & Qt::ShiftModifier) ? s : s.toLower();
}

}  // namespace

TerminalView::TerminalView(QWidget *parent) : QAbstractScrollArea(parent) {
  screen_ = new TermScreen(kDefaultRows, kDefaultCols, this);

  QFont menlo("Menlo", 13);  // what Menlo lacks comes from Qt's own fallback
  menlo.setStyleHint(QFont::TypeWriter);
  setFont(menlo);
  for (int i = 0; i < 4; ++i) {
    fonts_[i] = font();
    fonts_[i].setBold(i & 1);
    fonts_[i].setItalic(i & 2);
    rawFonts_[i] = QRawFont::fromFont(fonts_[i]);
    metrics_.emplace_back(fonts_[i]);
  }
  const QFontMetrics fm(fonts_[0]);
  cellW_ = std::max(1, fm.averageCharWidth());
  cellH_ = std::max(1, fm.lineSpacing());
  ascent_ = fm.ascent();
  underlineY_ = fm.ascent() + fm.underlinePos();
  lineWidth_ = std::max(1, fm.lineWidth());

  setFrameShape(QFrame::NoFrame);
  setViewportMargins(kPadding, kPadding, kPadding, kPadding);
  QPalette pal = palette();
  pal.setColor(QPalette::Window, kBackground);  // the padding
  pal.setColor(QPalette::Base, kBackground);
  setPalette(pal);
  setAutoFillBackground(true);
  viewport()->setAutoFillBackground(false);
  viewport()->setAttribute(Qt::WA_OpaquePaintEvent);  // paintEvent covers all it is asked to
  viewport()->setCursor(Qt::IBeamCursor);
  // always shown, so that the cells do not move when it comes to span the scrollback
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  syncScrollBar();
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_InputMethodEnabled);
  setAttribute(Qt::WA_NoMousePropagation);  // a wheel or click over the padding is not the window's

  repaint_.setSingleShot(true);
  repaint_.setInterval(kRepaintMs);
  connect(&repaint_, &QTimer::timeout, this, &TerminalView::repaintDamaged);
  dragScroll_.setInterval(kDragScrollMs);
  connect(&dragScroll_, &QTimer::timeout, this, &TerminalView::dragScroll);
  connect(screen_, &TermScreen::damaged, this, &TerminalView::screenDamaged);
  connect(screen_, &TermScreen::scrolled, this, &TerminalView::scrollbackChanged);
  connect(screen_, &TermScreen::altScreenChanged, this, &TerminalView::altScreenChanged);
  connect(screen_, &TermScreen::reply, this, [this](const QByteArray &bytes) {
    if (pty_) pty_->write(bytes);
  });
}

TerminalView::~TerminalView() { delete std::exchange(pty_, nullptr); }

TermScreen *TerminalView::screen() const { return screen_; }
bool TerminalView::running() const { return pty_ && pty_->running(); }
qint64 TerminalView::pid() const { return pty_ ? pty_->pid() : 0; }
int TerminalView::paints() const { return paints_; }
int TerminalView::rowsPainted() const { return rowsPainted_; }

bool TerminalView::run(const QString &program, const QStringList &args, const QStringList &env,
                       const QString &cwd) {
  if (pty_) return false;  // there is a Pty only while its program runs
  auto *pty = new Pty(this);
  if (!pty->start(program, args, env, cwd, screen_->rows(), screen_->cols())) {
    delete pty;
    return false;
  }
  pty_ = pty;
  typed_.clear();
  screen_->feed(kFreshTerminal);  // no output can have arrived yet: it comes through the event loop
  connect(pty, &Pty::output, screen_, &TermScreen::feed);
  connect(pty, &Pty::exited, this, [this, pty](int status) {
    pty_ = nullptr;
    pty->deleteLater();
    emit exited(status);
  });
  return true;
}

// --- size

void TerminalView::resizeEvent(QResizeEvent *) {
  fitToViewport();
  syncScrollBar();               // a page is the screen's rows
  tellInputMethodCursor(false);  // the cursor may have moved with nothing damaged at its new place
}

void TerminalView::fitToViewport() {
  const QSize area = viewport()->size();
  const int rows = std::max(kMinRows, area.height() / cellH_);
  const int cols = std::max(kMinCols, area.width() / cellW_);
  if (rows == screen_->rows() && cols == screen_->cols()) return;
  clearSelection();  // its cells are no longer where they were
  screen_->resize(rows, cols);
  if (pty_) pty_->resize(rows, cols);
  scrollbackChanged();  // a resize pushes lines into the scrollback, or takes them back
}

QRect TerminalView::cellRect(int row, int col, int width) const {
  return QRect(col * cellW_, row * cellH_, width * cellW_, cellH_);
}

QRect TerminalView::cursorRectangle() const {
  const QPoint c = screen_->cursor();
  return cellRect(c.y(), c.x()).translated(viewport()->pos());  // the viewport is inset
}

// --- looking back
//
// The view shows the screen's rows from -scrollPos_ on: row -1 is the newest line of the
// scrollback. The scroll bar runs from the oldest line (0) to the bottom (scrollLimit()). Lines are
// counted as TermPoint counts them, from the first line there ever was.

qint64 TerminalView::topLine() const {
  return screen_->linesDropped() + screen_->scrollback() - scrollPos_;
}

int TerminalView::viewRow(qint64 line) const {
  return static_cast<int>(std::clamp<qint64>(line - topLine(), -1, screen_->rows()));
}

int TerminalView::scrollLimit() const { return screen_->altScreen() ? 0 : screen_->scrollback(); }

void TerminalView::scrollTo(int linesBack) {
  linesBack = std::clamp(linesBack, 0, scrollLimit());
  if (linesBack == scrollPos_) return;
  scrollPos_ = linesBack;
  syncScrollBar();
  damageAll();
}

void TerminalView::syncScrollBar() {
  QScrollBar *bar = verticalScrollBar();
  const QSignalBlocker quiet(bar);  // the view's own moves: scrollContentsBy is for the person's
  const int limit = scrollLimit();
  bar->setRange(0, limit);
  bar->setPageStep(screen_->rows());
  bar->setValue(limit - scrollPos_);
}

// the person moved the scroll bar
void TerminalView::scrollContentsBy(int, int) {
  const QScrollBar *bar = verticalScrollBar();
  scrollTo(bar->maximum() - bar->value());
}

// lines went into the scrollback, or out of it (dropped past the limit, cleared, or taken back onto
// the screen by a resize). A view at the bottom stays there; one looking back keeps its lines in
// view, as far as there are lines to keep, and is repainted only when it could not (the lines it
// shows from the screen were damaged where they changed). A selection keeps its text until its
// first line has left.
void TerminalView::scrollbackChanged() {
  const int lines = screen_->scrollback();
  const qint64 dropped = screen_->linesDropped();
  if (!selection_.empty() && !selection_.everything() && selection_.first().line < dropped) clearSelection();
  if (scrollPos_ > 0) {
    const qint64 top = dropped_ + scrollback_ - scrollPos_;  // the line at the top of the view
    const qint64 back = dropped + lines - top;                // how far back it is now
    scrollPos_ = static_cast<int>(std::clamp<qint64>(back, 0, scrollLimit()));
    if (scrollPos_ != back) damageAll();
  }
  scrollback_ = lines;
  dropped_ = dropped;
  syncScrollBar();
}

// on or off the alternate screen: the view is at the bottom, with nothing selected
void TerminalView::altScreenChanged() {
  clearSelection();
  scrollPos_ = 0;
  syncScrollBar();
  damageAll();
}

// rows of the screen changed: a selection over them goes (everything selected stays), and they are
// painted where the view shows them
void TerminalView::screenDamaged(int firstRow, int lastRow) {
  if (!selection_.empty() && !selection_.everything() &&
      TermSelection::rowOf(*screen_, selection_.first().line) <= lastRow &&
      TermSelection::rowOf(*screen_, selection_.last().line) >= firstRow)
    clearSelection();
  damage(firstRow + scrollPos_, lastRow + scrollPos_);
}

// --- painting

void TerminalView::damage(int firstRow, int lastRow) {
  const int rows = screen_->rows();
  if (static_cast<int>(damaged_.size()) != rows) damaged_.resize(static_cast<size_t>(rows), false);
  firstRow = std::max(firstRow, 0);
  lastRow = std::min(lastRow, rows - 1);
  if (firstRow > lastRow) return;
  for (int r = firstRow; r <= lastRow; ++r) damaged_[static_cast<size_t>(r)] = true;
  if (!repaint_.isActive()) repaint_.start();
}

void TerminalView::damageCursorRow() {
  const int row = screen_->cursor().y() + scrollPos_;
  damage(row, row);
}

void TerminalView::damageAll() { damage(0, screen_->rows() - 1); }

void TerminalView::repaintDamaged() {
  QRegion region;
  const int width = viewport()->width();
  const int rows = static_cast<int>(damaged_.size());
  for (int r = 0; r < rows;) {
    if (!damaged_[static_cast<size_t>(r)]) {
      ++r;
      continue;
    }
    int end = r;
    while (end < rows && damaged_[static_cast<size_t>(end)]) damaged_[static_cast<size_t>(end++)] = false;
    region += QRect(0, r * cellH_, width, (end - r) * cellH_);
    r = end;
  }
  viewport()->update(region);
  tellInputMethodCursor(false);  // a cursor that moved damaged its rows
}

// a selected cell is drawn reversed
TerminalView::Style TerminalView::styleOf(const TermCell &cell, bool selected) const {
  Style s{cell.fg.isValid() ? cell.fg : kForeground, cell.bg.isValid() ? cell.bg : kBackground,
          cell.bold, cell.italic, cell.underline};
  if (cell.reverse != selected) std::swap(s.fg, s.bg);
  return s;
}

int TerminalView::fontIndex(const Style &style) { return (style.bold ? 1 : 0) | (style.italic ? 2 : 0); }
const QFont &TerminalView::fontFor(const Style &style) const { return fonts_[fontIndex(style)]; }

void TerminalView::paintEvent(QPaintEvent *e) {
  ++paints_;
  rowsPainted_ = 0;
  QPainter p(viewport());
  const QRegion &region = e->region();
  const int rows = screen_->rows(), width = viewport()->width();
  for (int row = 0; row < rows; ++row) {
    if (!region.intersects(QRect(0, row * cellH_, width, cellH_))) continue;
    paintRow(p, row);
    ++rowsPainted_;
  }
  p.setClipping(false);
  const QRect below(0, rows * cellH_, width, viewport()->height() - rows * cellH_);
  if (below.isValid() && region.intersects(below)) p.fillRect(below, kBackground);
}

// a row of the view as runs of cells that look the same, the cursor or the composition on it. The
// cursor is drawn only while the view is at the bottom
void TerminalView::paintRow(QPainter &p, int row) {
  const int cols = screen_->cols();
  const qint64 line = topLine() + row;
  rowCells_.resize(static_cast<size_t>(cols));
  for (int col = 0; col < cols; ++col) rowCells_[static_cast<size_t>(col)] = screen_->cell(row - scrollPos_, col);
  const std::vector<TermCell> &cells = rowCells_;
  auto styleAt = [&](int col) { return styleOf(cells[static_cast<size_t>(col)], selection_.contains({line, col})); };
  const QRect strip(0, row * cellH_, viewport()->width(), cellH_);
  p.setClipRect(strip);  // a glyph that reaches past its row would not be repainted with that row
  Style style = styleAt(0);
  for (int col = 0; col < cols;) {
    int end = col + 1;
    Style next;
    for (; end < cols; ++end) {
      if (cells[static_cast<size_t>(end)].width == 0) continue;  // goes with its first half
      next = styleAt(end);
      if (!(next == style)) break;
    }
    paintCells(p, row, col, end, style, cells);
    col = end;
    style = next;
  }
  const int right = cols * cellW_;
  if (right < strip.width()) p.fillRect(QRect(right, strip.top(), strip.width() - right, cellH_), kBackground);
  if (scrollPos_ > 0 || row != screen_->cursor().y()) return;
  if (!composing_.isEmpty()) paintComposition(p, row);
  else if (screen_->cursorVisible()) paintCursor(p, row, cells);
}

// cells from..to-1 of a row in one style: one rectangle of background, the text, the underline
void TerminalView::paintCells(QPainter &p, int row, int from, int to, const Style &style,
                              const std::vector<TermCell> &cells) {
  const QRect rect = cellRect(row, from, to - from);
  p.fillRect(rect, style.bg);
  paintText(p, row, from, to, style, cells);
  if (style.underline) p.fillRect(QRect(rect.left(), rect.top() + underlineY_, rect.width(), lineWidth_), style.fg);
}

// the text of cells from..to-1. What Menlo has, one character to a cell, is drawn at once, each
// glyph at its cell's column. The rest (a character from a fallback font, a wide one, one with
// combining marks) is drawn a cell at a time: its own advance would carry what follows off the grid.
void TerminalView::paintText(QPainter &p, int row, int from, int to, const Style &style,
                             const std::vector<TermCell> &cells) {
  QString chars;          // the cells of one character, which Menlo may have
  std::vector<int> charCols;
  std::vector<int> others;  // cells drawn one at a time
  for (int col = from; col < to; ++col) {
    const TermCell &cell = cells[static_cast<size_t>(col)];
    if (cell.width == 0 || cell.text.isEmpty() || cell.text == QLatin1String(" ")) continue;
    if (cell.width == 1 && cell.text.size() == 1) {
      chars += cell.text;
      charCols.push_back(col);
    } else {
      others.push_back(col);
    }
  }
  const int baseline = row * cellH_ + ascent_;
  p.setPen(style.fg);
  if (!chars.isEmpty()) {
    const QRawFont &raw = rawFonts_[fontIndex(style)];
    const QList<quint32> glyphs = raw.glyphIndexesForString(chars);
    QList<quint32> drawn;
    QList<QPointF> positions;
    for (qsizetype i = 0; i < chars.size(); ++i) {
      const int col = charCols[static_cast<size_t>(i)];
      if (i < glyphs.size() && glyphs[i] != 0) {  // 0: Menlo has no such glyph
        drawn << glyphs[i];
        positions << QPointF(col * cellW_, baseline);
      } else {
        others.push_back(col);
      }
    }
    if (!drawn.isEmpty()) {
      QGlyphRun run;
      run.setRawFont(raw);
      run.setGlyphIndexes(drawn);
      run.setPositions(positions);
      p.drawGlyphRun(QPointF(), run);
    }
  }
  if (others.empty()) return;
  p.setFont(fontFor(style));
  const QFontMetricsF &fm = metrics_[static_cast<size_t>(fontIndex(style))];
  for (const int col : others) {
    const TermCell &cell = cells[static_cast<size_t>(col)];
    qreal x = col * cellW_;
    if (cell.width == 2) x += (2 * cellW_ - fm.horizontalAdvance(cell.text)) / 2;  // centred in its two cells
    p.drawText(QPointF(x, baseline), cell.text);
  }
}

// a block with the character in the background colour when the view has the focus, an outline
// when it does not
void TerminalView::paintCursor(QPainter &p, int row, const std::vector<TermCell> &cells) {
  const int cols = static_cast<int>(cells.size());
  int col = std::clamp(screen_->cursor().x(), 0, cols - 1);
  if (cells[static_cast<size_t>(col)].width == 0 && col > 0) --col;  // all of a wide character
  const int to = std::min(cols, col + std::max(cells[static_cast<size_t>(col)].width, 1));
  Style style = styleOf(cells[static_cast<size_t>(col)], false);
  if (hasFocus()) {
    std::swap(style.fg, style.bg);
    paintCells(p, row, col, to, style, cells);
  } else {
    p.setPen(style.fg);
    p.setBrush(Qt::NoBrush);
    p.drawRect(cellRect(row, col, to - col).adjusted(0, 0, -1, -1));
  }
}

// what the input method is composing, over the cells from the cursor on, underlined
void TerminalView::paintComposition(QPainter &p, int row) {
  const int x = screen_->cursor().x() * cellW_;
  const int width = qCeil(metrics_[0].horizontalAdvance(composing_));
  const int cells = std::max(1, (width + cellW_ - 1) / cellW_);
  p.fillRect(cellRect(row, screen_->cursor().x(), cells), kBackground);
  p.setPen(kForeground);
  p.setFont(fonts_[0]);
  p.drawText(QPointF(x, row * cellH_ + ascent_), composing_);
  p.fillRect(QRect(x, row * cellH_ + underlineY_, width, lineWidth_), kForeground);
}

void TerminalView::focusInEvent(QFocusEvent *) { damageCursorRow(); }

// the release of a button held may never come (a dialog opened, another application came forward):
// a drag ends here
void TerminalView::focusOutEvent(QFocusEvent *) {
  damageCursorRow();
  dragScroll_.stop();
  mouseOwner_ = MouseOwner::None;
}

// --- selecting

QString TerminalView::selectedText() const { return selection_.text(*screen_); }

void TerminalView::copy() {
  const QString text = selectedText();
  if (!text.isEmpty()) QGuiApplication::clipboard()->setText(text);
}

void TerminalView::paste() {
  const QString text = QGuiApplication::clipboard()->text();
  if (text.isEmpty()) return;
  typed_.clear();  // nothing an input method could take back
  scrollTo(0);
  screen_->paste(text);
}

TermPoint TerminalView::pointAt(QPoint pos) const {
  const int row = std::clamp(pos.y() / cellH_, 0, screen_->rows() - 1);
  const int col = std::clamp(pos.x() / cellW_, 0, screen_->cols() - 1);
  return {topLine() + row, col};
}

void TerminalView::startSelection(TermPoint p, TermSelection::Unit unit) {
  const TermSelection before = selection_;
  selection_.start(*screen_, p, unit);
  selectionChanged(before);
}

void TerminalView::extendSelection(TermPoint p) {
  const TermSelection before = selection_;
  selection_.extend(*screen_, p);
  selectionChanged(before);
}

void TerminalView::selectAll() {
  const TermSelection before = selection_;
  selection_.all();
  selectionChanged(before);
}

void TerminalView::clearSelection() {
  const TermSelection before = selection_;
  selection_.clear();
  selectionChanged(before);
}

// the rows the selection covered and covers are painted again
void TerminalView::selectionChanged(const TermSelection &before) {
  for (const TermSelection *s : {&before, &std::as_const(selection_)}) {
    if (s->everything()) damageAll();
    else if (!s->empty()) damage(viewRow(s->first().line), viewRow(s->last().line));
  }
}

// while a selecting drag is held past the top or the bottom, the view scrolls (the further out, the
// faster) and the selection follows
void TerminalView::dragScroll() {
  const int y = dragPos_.y(), height = viewport()->height();
  const int lines = y < 0 ? 1 + -y / cellH_ : y >= height ? -(1 + (y - height) / cellH_) : 0;
  if (lines == 0 || mouseOwner_ != MouseOwner::View) {
    dragScroll_.stop();
    return;
  }
  scrollTo(scrollPos_ + lines);
  extendSelection(pointAt(dragPos_));
}

// --- the mouse: the program's when it asked for it, the screen is in view and Option is not held;
// the view's (to select) otherwise

TerminalView::MouseOwner TerminalView::takeMouse(Qt::KeyboardModifiers mods) {
  if (mouseOwner_ == MouseOwner::None) {
    const bool program = screen_->mouseWanted() && scrollPos_ == 0 && !(mods & Qt::AltModifier);
    mouseOwner_ = program ? MouseOwner::Program : MouseOwner::View;
  }
  return mouseOwner_;
}

QPoint TerminalView::screenCellAt(QPoint pos) const {
  const TermPoint p = pointAt(pos);
  return QPoint(p.col, std::max(0, TermSelection::rowOf(*screen_, p.line)));
}

void TerminalView::reportMouse(QMouseEvent *e, bool pressed) {
  const QPoint cell = screenCellAt(e->position().toPoint());
  screen_->mouse(cell.y(), cell.x(), e->button(), pressed, translate(e->modifiers()).terminal);
}

void TerminalView::mousePressEvent(QMouseEvent *e) {
  e->accept();
  const bool program = takeMouse(e->modifiers()) == MouseOwner::Program;
  if (program || e->button() != Qt::LeftButton) {
    clearSelection();  // a press ends a selection, whoever it is for (the view's left one starts another)
    if (program) reportMouse(e, true);
    return;
  }
  dragPos_ = e->position().toPoint();
  const TermPoint p = pointAt(dragPos_);
  // a third press soon after a double click, on its cell: the row
  const bool third = sinceDoubleClick_.isValid() && p == doubleClickAt_ &&
                     !sinceDoubleClick_.hasExpired(QGuiApplication::styleHints()->mouseDoubleClickInterval());
  sinceDoubleClick_.invalidate();
  startSelection(p, third ? TermSelection::Unit::Line : TermSelection::Unit::Cell);
}

// the second press of a double click
void TerminalView::mouseDoubleClickEvent(QMouseEvent *e) {
  e->accept();
  if (takeMouse(e->modifiers()) == MouseOwner::Program) {
    reportMouse(e, true);
    return;
  }
  if (e->button() != Qt::LeftButton) return;
  dragPos_ = e->position().toPoint();
  doubleClickAt_ = pointAt(dragPos_);
  sinceDoubleClick_.start();
  startSelection(doubleClickAt_, TermSelection::Unit::Word);
}

void TerminalView::mouseMoveEvent(QMouseEvent *e) {
  e->accept();
  const QPoint pos = e->position().toPoint();
  if (mouseOwner_ == MouseOwner::Program) {
    const QPoint cell = screenCellAt(pos);
    screen_->mouseMove(cell.y(), cell.x(), translate(e->modifiers()).terminal);
  } else if (mouseOwner_ == MouseOwner::View && (e->buttons() & Qt::LeftButton)) {
    dragPos_ = pos;
    extendSelection(pointAt(pos));
    if (pos.y() >= 0 && pos.y() < viewport()->height()) dragScroll_.stop();
    else if (!dragScroll_.isActive()) dragScroll_.start();
  }
}

void TerminalView::mouseReleaseEvent(QMouseEvent *e) {
  e->accept();
  if (mouseOwner_ == MouseOwner::Program) {
    reportMouse(e, false);
  } else if (mouseOwner_ == MouseOwner::View && e->button() == Qt::LeftButton) {
    extendSelection(pointAt(e->position().toPoint()));
    dragScroll_.stop();
  }
  if (e->buttons() == Qt::NoButton) mouseOwner_ = MouseOwner::None;
}

// The wheel looks back while the view is looking back. At the bottom it is a program's that asked
// for the mouse, a step a notch at the cell under the pointer; on the alternate screen, with nothing
// to look back at, the program's Up and Down; else it looks back.
//
// It turns in lines: a trackpad (a device whose turns come in phases) a line for each cell's height
// of its pixels, a wheel three lines a notch of its angle (macOS gives a wheel pixels too, at its own
// rate, which are not used). What is less than a step is kept for the next turn, if that goes the
// same way and does the same.
void TerminalView::wheelEvent(QWheelEvent *e) {
  e->accept();
  const WheelGoes goes = scrollPos_ > 0           ? WheelGoes::View
                         : screen_->mouseWanted() ? WheelGoes::Program
                         : screen_->altScreen()   ? WheelGoes::Keys
                                                  : WheelGoes::View;
  const bool trackpad = e->phase() != Qt::NoScrollPhase && !e->pixelDelta().isNull();
  const qreal turned = trackpad ? qreal(e->pixelDelta().y()) / cellH_
                                : qreal(e->angleDelta().y()) * kLinesPerNotch / kWheelNotch;
  if (goes != wheelGoes_ || turned * wheelLines_ < 0) wheelLines_ = 0;
  wheelGoes_ = goes;
  wheelLines_ += turned;
  const int lines = static_cast<int>(wheelLines_);  // toward zero
  switch (goes) {
    case WheelGoes::Program: {
      const int steps = lines / kLinesPerNotch;
      wheelLines_ -= steps * kLinesPerNotch;
      const QPoint cell = screenCellAt(e->position().toPoint());
      screen_->wheel(cell.y(), cell.x(), steps);
      break;
    }
    case WheelGoes::Keys:
      wheelLines_ -= lines;
      for (int i = 0; i < std::abs(lines); ++i) screen_->key(lines > 0 ? Qt::Key_Up : Qt::Key_Down, Qt::NoModifier, {});
      break;
    case WheelGoes::View:
      wheelLines_ -= lines;
      scrollTo(scrollPos_ + lines);
      break;
  }
}

// --- keys

bool TerminalView::focusNextPrevChild(bool) { return false; }

bool TerminalView::event(QEvent *e) {
  // the application's shortcuts get no key the program would: only Command ones
  if (e->type() == QEvent::ShortcutOverride) {
    auto *k = static_cast<QKeyEvent *>(e);
    const KeyMods mods = translate(k->modifiers());
    if (!mods.command || viewCommand(k->key(), mods)) {
      e->accept();
      return true;
    }
  }
  return QAbstractScrollArea::event(e);
}

void TerminalView::keyPressEvent(QKeyEvent *e) {
  if (sendKey(e)) e->accept();
  else e->ignore();
}

// a key press to TermScreen::key in the terminal's terms, or to the view's own copy, paste and
// select all. false when it is neither: a Command combination the application may have a use for
bool TerminalView::sendKey(QKeyEvent *e) {
  const KeyMods mods = translate(e->modifiers());
  if (!modifierKey(e->key())) typed_.clear();
  if (mods.command) {
    if (!viewCommand(e->key(), mods)) return false;
    if (e->key() == Qt::Key_C) copy();
    else if (e->key() == Qt::Key_V) paste();
    else selectAll();
    return true;
  }
  if (!modifierKey(e->key())) scrollTo(0);  // typing is done at the bottom
  QString text = e->text();
  // Option is Meta: the key's own character goes out with ESC before it, not the one Option
  // composes (Option+b is ESC b, not ∫). A key that makes no character keeps its text.
  if (mods.terminal & Qt::AltModifier) {
    const QString own = keyCharacter(e->key(), mods.terminal);
    if (!own.isEmpty()) text = own;
  }
  screen_->key(e->key(), mods.terminal, text);
  if (!(mods.terminal & (Qt::ControlModifier | Qt::AltModifier)) && plainText(text)) typed_ = text;
  return true;
}

void TerminalView::inputMethodEvent(QInputMethodEvent *e) {
  if (!e->commitString().isEmpty() || !e->preeditString().isEmpty()) scrollTo(0);
  // the input method may take back what was just typed: the press-and-hold menu replaces the letter
  // it was opened on. When what it replaces ends at the cursor, is within what the view itself sent
  // last and does not cut a character in two, it is erased with Backspace, one per character.
  // Anything else (text the program drew, or that the view did not send) cannot be replaced in a
  // terminal: the commit goes alone.
  const int start = e->replacementStart(), length = e->replacementLength();
  if (length > 0 && start + length == 0 && length <= typed_.size() &&
      !typed_.at(typed_.size() - length).isLowSurrogate()) {
    const qsizetype count = typed_.right(length).toUcs4().size();
    for (qsizetype i = 0; i < count; ++i) screen_->key(Qt::Key_Backspace, Qt::NoModifier, {});
    typed_.chop(length);
  }
  if (!e->commitString().isEmpty()) {
    screen_->key(0, Qt::NoModifier, e->commitString());
    typed_ = e->commitString();
  }
  if (e->preeditString() != composing_) {
    composing_ = e->preeditString();
    damageCursorRow();
    tellInputMethodCursor(true);
  }
  e->accept();
}

// while the view has the focus and is at the bottom, the input method's picker and candidates
// follow the cursor: told when the cursor's cell has moved, or always (the composition changed)
void TerminalView::tellInputMethodCursor(bool always) {
  if (!hasFocus() || scrollPos_ > 0) return;
  const QRect cursor = cursorRectangle();
  if (!always && cursor == imCursor_) return;
  imCursor_ = cursor;
  QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
}

QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery query) const {
  switch (query) {
    case Qt::ImEnabled:
      // Option is Meta: while it is held the key must come as a key press, not go to macOS for
      // composing (Option+e would start an accent)
      return !(QGuiApplication::queryKeyboardModifiers() & Qt::AltModifier);
    case Qt::ImCursorRectangle:  // where a picker opens
      return cursorRectangle();
    case Qt::ImHints:
      return static_cast<int>(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText);
    case Qt::ImFont:
      return fonts_[0];
    case Qt::ImSurroundingText:
      return textBeforeCursor();
    case Qt::ImCursorPosition:
    case Qt::ImAnchorPosition:
      return static_cast<int>(textBeforeCursor().size());
    default:
      return QAbstractScrollArea::inputMethodQuery(query);
  }
}

// the cursor's row up to the cursor: what an input method sees as typed so far
QString TerminalView::textBeforeCursor() const {
  const QPoint c = screen_->cursor();
  QString s;
  for (int col = 0; col < c.x(); ++col) {
    const TermCell cell = screen_->cell(c.y(), col);
    if (cell.width == 0) continue;
    s += cell.text.isEmpty() ? QStringLiteral(" ") : cell.text;
  }
  return s;
}
