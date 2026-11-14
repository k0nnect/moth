#include "terminal_view.hpp"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QGlyphRun>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScrollBar>
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

// fed to the screen before a new program: off the alternate screen and mouse reports off (libvterm's
// reset leaves both as they were), a full reset, the scrollback cleared, the title emptied
const QByteArray kFreshTerminal =
    "\x1b[?1049l\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b" "c\x1b[3J\x1b]2;\x1b\\";

// a key's modifiers in the terminal's terms. On macOS Qt reports the Control key as Meta and
// Command as Control (unless the application asks it not to swap them); Option is Alt either way.
struct KeyMods {
  bool command = false;          // a key with Command is never the program's
  Qt::KeyboardModifiers terminal;  // Control, Alt (Option) and Shift as TermScreen::key reads them
};

KeyMods translate(Qt::KeyboardModifiers qt) {
  KeyMods m;
  m.terminal = qt & (Qt::ShiftModifier | Qt::AltModifier | Qt::KeypadModifier);
#ifdef Q_OS_MACOS
  const bool swapped = !QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
  m.command = qt & (swapped ? Qt::ControlModifier : Qt::MetaModifier);
  if (qt & (swapped ? Qt::MetaModifier : Qt::ControlModifier)) m.terminal |= Qt::ControlModifier;
#else
  m.terminal |= qt & Qt::ControlModifier;
#endif
  return m;
}

// Command combinations the view keeps for itself (copy, paste, select all); they send nothing
bool viewCommand(int key) { return key == Qt::Key_C || key == Qt::Key_V || key == Qt::Key_A; }

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
  // always shown, so that the cells do not move when it comes to span the scrollback
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  verticalScrollBar()->setRange(0, 0);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_InputMethodEnabled);

  repaint_.setSingleShot(true);
  repaint_.setInterval(kRepaintMs);
  connect(&repaint_, &QTimer::timeout, this, &TerminalView::repaintDamaged);
  connect(screen_, &TermScreen::damaged, this, &TerminalView::damage);
  connect(screen_, &TermScreen::reply, this, [this](const QByteArray &bytes) {
    if (pty_) pty_->write(bytes);
  });
}

TerminalView::~TerminalView() { delete std::exchange(pty_, nullptr); }

TermScreen *TerminalView::screen() const { return screen_; }
bool TerminalView::running() const { return pty_ && pty_->running(); }
qint64 TerminalView::pid() const { return pty_ ? pty_->pid() : 0; }
int TerminalView::paints() const { return paints_; }

bool TerminalView::run(const QString &program, const QStringList &args, const QStringList &env,
                       const QString &cwd) {
  if (pty_) return false;  // there is a Pty only while its program runs
  auto *pty = new Pty(this);
  if (!pty->start(program, args, env, cwd, screen_->rows(), screen_->cols())) {
    delete pty;
    return false;
  }
  pty_ = pty;
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

void TerminalView::resizeEvent(QResizeEvent *) { fitToViewport(); }

void TerminalView::fitToViewport() {
  const QSize area = viewport()->size();
  const int rows = std::max(kMinRows, area.height() / cellH_);
  const int cols = std::max(kMinCols, area.width() / cellW_);
  if (rows == screen_->rows() && cols == screen_->cols()) return;
  screen_->resize(rows, cols);
  if (pty_) pty_->resize(rows, cols);
}

QRect TerminalView::cellRect(int row, int col, int width) const {
  return QRect(col * cellW_, row * cellH_, width * cellW_, cellH_);
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
}

TerminalView::Style TerminalView::styleOf(const TermCell &cell) const {
  Style s{cell.fg.isValid() ? cell.fg : kForeground, cell.bg.isValid() ? cell.bg : kBackground,
          cell.bold, cell.italic, cell.underline};
  if (cell.reverse) std::swap(s.fg, s.bg);
  return s;
}

int TerminalView::fontIndex(const Style &style) { return (style.bold ? 1 : 0) | (style.italic ? 2 : 0); }
const QFont &TerminalView::fontFor(const Style &style) const { return fonts_[fontIndex(style)]; }

void TerminalView::paintEvent(QPaintEvent *e) {
  ++paints_;
  QPainter p(viewport());
  const QRegion &region = e->region();
  const int rows = screen_->rows(), width = viewport()->width();
  for (int row = 0; row < rows; ++row)
    if (region.intersects(QRect(0, row * cellH_, width, cellH_))) paintRow(p, row);
  p.setClipping(false);
  const QRect below(0, rows * cellH_, width, viewport()->height() - rows * cellH_);
  if (below.isValid() && region.intersects(below)) p.fillRect(below, kBackground);
}

// a row as runs of cells that look the same, the cursor or the composition on it
void TerminalView::paintRow(QPainter &p, int row) {
  const int cols = screen_->cols();
  std::vector<TermCell> cells(static_cast<size_t>(cols));
  for (int col = 0; col < cols; ++col) cells[static_cast<size_t>(col)] = screen_->cell(row, col);
  const QRect line(0, row * cellH_, viewport()->width(), cellH_);
  p.setClipRect(line);  // a glyph that reaches past its row would not be repainted with that row
  for (int col = 0; col < cols;) {
    const Style style = styleOf(cells[static_cast<size_t>(col)]);
    int end = col + 1;
    // the second half of a wide character goes with its first, whatever it says itself
    while (end < cols && (cells[static_cast<size_t>(end)].width == 0 ||
                          styleOf(cells[static_cast<size_t>(end)]) == style))
      ++end;
    paintCells(p, row, col, end, style, cells);
    col = end;
  }
  const int right = cols * cellW_;
  if (right < line.width()) p.fillRect(QRect(right, line.top(), line.width() - right, cellH_), kBackground);
  if (row != screen_->cursor().y()) return;
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
  const QFontMetricsF fm(fontFor(style));
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
  Style style = styleOf(cells[static_cast<size_t>(col)]);
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
  const int width = QFontMetrics(fonts_[0]).horizontalAdvance(composing_);
  const int cells = std::max(1, (width + cellW_ - 1) / cellW_);
  p.fillRect(cellRect(row, screen_->cursor().x(), cells), kBackground);
  p.setPen(kForeground);
  p.setFont(fonts_[0]);
  p.drawText(QPointF(x, row * cellH_ + ascent_), composing_);
  p.fillRect(QRect(x, row * cellH_ + underlineY_, width, lineWidth_), kForeground);
}

void TerminalView::focusInEvent(QFocusEvent *) { damage(screen_->cursor().y(), screen_->cursor().y()); }
void TerminalView::focusOutEvent(QFocusEvent *) { damage(screen_->cursor().y(), screen_->cursor().y()); }

// --- keys

bool TerminalView::focusNextPrevChild(bool) { return false; }

bool TerminalView::event(QEvent *e) {
  // the application's shortcuts get no key the program would: only Command ones
  if (e->type() == QEvent::ShortcutOverride) {
    auto *k = static_cast<QKeyEvent *>(e);
    if (!translate(k->modifiers()).command || viewCommand(k->key())) {
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

// a key press to TermScreen::key in the terminal's terms. false when it is not the terminal's: a
// Command combination the application may have a use for
bool TerminalView::sendKey(QKeyEvent *e) {
  const KeyMods mods = translate(e->modifiers());
  if (mods.command) return viewCommand(e->key());
  QString text = e->text();
  // Option is Meta: the key's own character goes out with ESC before it, not the one Option
  // composes (Option+b is ESC b, not ∫). A key that makes no character keeps its text.
  if (mods.terminal & Qt::AltModifier) {
    const QString own = keyCharacter(e->key(), mods.terminal);
    if (!own.isEmpty()) text = own;
    // libvterm sends Alt with a character past ASCII as ESC and one byte of the character, not its
    // UTF-8 (Option+ö on a German keyboard): ESC goes as a key of its own, then the character
    const bool pastAscii = std::any_of(text.begin(), text.end(), [](QChar c) { return c.unicode() >= 0x80; });
    if (pastAscii && !(mods.terminal & Qt::ControlModifier)) {
      screen_->key(Qt::Key_Escape, Qt::NoModifier, {});
      screen_->key(0, mods.terminal & ~Qt::AltModifier, text);
      return true;
    }
  }
  screen_->key(e->key(), mods.terminal, text);
  return true;
}

void TerminalView::inputMethodEvent(QInputMethodEvent *e) {
  // what the input method takes back from just before the cursor (the press-and-hold menu replaces
  // the letter it was opened on) is erased with Backspace, one per character; a terminal cannot
  // replace text anywhere else
  const int start = e->replacementStart(), length = e->replacementLength();
  if (length > 0 && start + length == 0) {
    const QString gone = textBeforeCursor().right(length);
    const qsizetype count = gone.size() == length ? gone.toUcs4().size() : length;
    for (qsizetype i = 0; i < count; ++i) screen_->key(Qt::Key_Backspace, Qt::NoModifier, {});
  }
  if (!e->commitString().isEmpty()) screen_->key(0, Qt::NoModifier, e->commitString());
  if (e->preeditString() != composing_) {
    composing_ = e->preeditString();
    damage(screen_->cursor().y(), screen_->cursor().y());
  }
  e->accept();
}

QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery query) const {
  switch (query) {
    case Qt::ImEnabled:
      // Option is Meta: while it is held the key must come as a key press, not go to macOS for
      // composing (Option+e would start an accent)
      return !(QGuiApplication::queryKeyboardModifiers() & Qt::AltModifier);
    case Qt::ImCursorRectangle: {  // where a picker opens: in this widget, the viewport is inset
      const QPoint c = screen_->cursor();
      return cellRect(c.y(), c.x()).translated(viewport()->pos());
    }
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
