#pragma once

#include <QAbstractScrollArea>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QRawFont>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <vector>

class Pty;
class TermScreen;
struct TermCell;

// a terminal: a TermScreen painted, a Pty behind it, the keyboard in front
//
// The screen is drawn in cells of Menlo 13 (the font's average advance by its line height), as many
// as fit in the viewport and never fewer than 2 rows by 10 columns; 24 by 80 until it is first
// sized. Only rows the screen reports damaged are repainted, at most once every 16 ms. Every key
// press goes to TermScreen::key, with macOS's modifiers turned into the terminal's: Control is
// Qt's Meta, Option is Meta (ESC before the key's own character, not what Option composes), and a
// key with Command is never sent. Text from an input method is sent as text when it is committed;
// what is being composed is drawn at the cursor.
class TerminalView : public QAbstractScrollArea {
  Q_OBJECT
public:
  explicit TerminalView(QWidget *parent = nullptr);
  ~TerminalView() override;  // the program is hung up on, and reaped
  TermScreen *screen() const;
  // starts the program at the view's size in cells; false when it cannot be run
  bool run(const QString &program, const QStringList &args, const QStringList &env, const QString &cwd);
  bool running() const;
  qint64 pid() const;
  int paints() const;        // how many times it has painted, for tests
  int rowsPainted() const;   // how many rows the last paint drew, for tests
  QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
signals:
  void exited(int status);

protected:
  bool event(QEvent *e) override;
  void paintEvent(QPaintEvent *e) override;
  void resizeEvent(QResizeEvent *e) override;
  void keyPressEvent(QKeyEvent *e) override;
  void inputMethodEvent(QInputMethodEvent *e) override;
  void focusInEvent(QFocusEvent *e) override;
  void focusOutEvent(QFocusEvent *e) override;
  bool focusNextPrevChild(bool next) override;  // Tab is the program's

private:
  // how a run of cells is drawn: the colours resolved (defaults filled in, reverse applied)
  struct Style {
    QColor fg, bg;
    bool bold = false, italic = false, underline = false;
    bool operator==(const Style &) const = default;
  };

  // size
  void fitToViewport();
  QRect cellRect(int row, int col, int width = 1) const;  // in the viewport
  QRect cursorRectangle() const;                          // the cursor's cell, in this widget

  // painting
  void damage(int firstRow, int lastRow);
  void repaintDamaged();
  Style styleOf(const TermCell &cell) const;
  static int fontIndex(const Style &style);  // into fonts_ and rawFonts_
  const QFont &fontFor(const Style &style) const;
  void paintRow(QPainter &p, int row);
  void paintCells(QPainter &p, int row, int from, int to, const Style &style, const std::vector<TermCell> &cells);
  void paintText(QPainter &p, int row, int from, int to, const Style &style, const std::vector<TermCell> &cells);
  void paintCursor(QPainter &p, int row, const std::vector<TermCell> &cells);
  void paintComposition(QPainter &p, int row);

  // keys
  bool sendKey(QKeyEvent *e);
  void tellInputMethodCursor(bool always);
  QString textBeforeCursor() const;

  TermScreen *screen_;
  Pty *pty_ = nullptr;
  QFont fonts_[4];             // regular, bold, italic, bold italic
  QRawFont rawFonts_[4];       // the same, Menlo's own glyphs only
  std::vector<QFontMetricsF> metrics_;  // the same, fallback fonts' advances included
  int cellW_ = 1, cellH_ = 1, ascent_ = 0, underlineY_ = 0, lineWidth_ = 1;
  std::vector<bool> damaged_;  // rows waiting to be repainted
  QTimer repaint_;
  std::vector<TermCell> rowCells_;  // the row being painted
  int paints_ = 0, rowsPainted_ = 0;
  QString composing_;          // the input method's text not yet committed
  QRect imCursor_;             // where the input method was last told the cursor is
  QString typed_;              // what the last key press or commit sent, as far as not taken back
};
