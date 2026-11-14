#pragma once

#include <QByteArray>
#include <QColor>
#include <QObject>
#include <QPoint>
#include <QString>
#include <deque>
#include <vector>
#include <vterm.h>

struct TermCell {
  QString text;                // "" for an empty cell and for the second half of a wide one
  QColor fg, bg;               // invalid QColor: the terminal's default, which the painter chooses
  bool bold = false, italic = false, underline = false, reverse = false;
  int width = 1;               // 1, 2, or 0 for the second half of a wide character
};

// what the terminal shows: libvterm's screen, its scrollback, and what the keyboard and mouse
// send back. no pty and no painting here.
//
// Rows and columns are 0-based. Everything the program must receive (answers to its queries, keys,
// paste, mouse reports) comes out of `reply`, in order; the owner writes it to the pty.
//
// Modifiers: key() and mouse() take Qt's flags where Qt::ControlModifier MEANS the terminal's
// Control and Qt::AltModifier means Option/Alt. They do not guess at the platform: on macOS Qt
// reports the Control key as Qt::MetaModifier and Command as Qt::ControlModifier (unless
// Qt::AA_MacDontSwapCtrlAndMeta is set), so the widget translates before calling. Likewise, when
// Option composes a character (Option+e), the widget passes the text it wants sent.
class TermScreen : public QObject {
  Q_OBJECT
public:
  static constexpr int kScrollbackLimit = 10000;  // lines kept after they scroll off the top

  TermScreen(int rows, int cols, QObject *parent = nullptr);
  ~TermScreen() override;
  void feed(const QByteArray &bytes);            // what the program printed
  int rows() const;
  int cols() const;
  void resize(int rows, int cols);
  int scrollback() const;                        // lines scrolled off the top, 10,000 at most
  // lines that have left the scrollback at its old end since the screen was made, pushed out past
  // the limit or cleared. With scrollback() it says how far every line has moved.
  qint64 linesDropped() const;
  // rows 0 .. rows()-1 are the live screen; row -1 is the newest scrolled-off line, -2 the one
  // before. Out of range gives an empty cell.
  TermCell cell(int row, int col) const;
  QString text(int row) const;                   // a row's text, trailing blanks dropped
  // the row goes on in the next: text came to its end and went on, rather than a new line starting.
  // Live rows have it from libvterm; a line in the scrollback has what could be read as it went
  // (see pushedLineWrapped)
  bool wrapped(int row) const;
  QPoint cursor() const;                         // x is the column, y the row
  bool cursorVisible() const;
  bool altScreen() const;
  bool mouseWanted() const;                      // the program asked for mouse reports
  QString title() const;
  void key(int qtKey, Qt::KeyboardModifiers mods, const QString &text);
  void paste(const QString &text);               // bracketed when the program asked for it
  void mouse(int row, int col, Qt::MouseButton button, bool pressed, Qt::KeyboardModifiers mods);
  // the pointer is over this cell: reported when the program asked for drags and a button is held
  // (or for every move); nothing unless mouseWanted()
  void mouseMove(int row, int col, Qt::KeyboardModifiers mods);
  void wheel(int row, int col, int steps);       // positive is up; sent only when mouseWanted()
signals:
  void damaged(int firstRow, int lastRow);
  void scrolled();                               // the scrollback grew, or was cleared
  void reply(const QByteArray &bytes);           // what must be written to the program
  void titleChanged();
  void bell();

private:
  struct Line {
    std::vector<VTermScreenCell> cells;          // trailing blank cells are not stored
    bool wrapped = false;                        // it went on in the next line
  };

  static int onDamage(VTermRect rect, void *self);
  static int onSetTermProp(VTermProp prop, VTermValue *val, void *self);
  static int onBell(void *self);
  static int onPushLine(int cols, const VTermScreenCell *cells, void *self);
  static int onPopLine(int cols, VTermScreenCell *cells, void *self);
  static int onSbClear(void *self);
  static void onOutput(const char *s, size_t len, void *self);

  TermCell convert(const VTermScreenCell &c) const;
  QColor colour(const VTermColor &c) const;
  VTermScreenCell blank() const;
  bool pushedLineWrapped();

  VTerm *vt_ = nullptr;
  VTermScreen *screen_ = nullptr;
  VTermState *state_ = nullptr;
  std::deque<Line> history_;                     // oldest first
  qint64 dropped_ = 0;                           // lines gone from the front of the history
  bool pushed_ = false;                          // a line went to the history during this feed
  int resizingFrom_ = 0;                         // during a resize the rows there were; 0 otherwise
  int resizePushes_ = 0;                         // lines the resize has pushed so far
  bool cursorVisible_ = true, altScreen_ = false, mouse_ = false;
  QString title_, titleBuf_;
};
