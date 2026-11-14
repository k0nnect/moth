#include "term_screen.hpp"

#include <algorithm>
#include <cstring>

namespace {

// the sixteen terminal colours (the xterm defaults), then the 6x6x6 cube and the grey ramp
QColor indexed(int i) {
  static const unsigned char ansi[16][3] = {
      {0, 0, 0},       {205, 0, 0},     {0, 205, 0},     {205, 205, 0},
      {0, 0, 238},     {205, 0, 205},   {0, 205, 205},   {229, 229, 229},
      {127, 127, 127}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
      {92, 92, 255},   {255, 0, 255},   {0, 255, 255},   {255, 255, 255}};
  if (i < 0 || i > 255) return QColor();
  if (i < 16) return QColor(ansi[i][0], ansi[i][1], ansi[i][2]);
  if (i < 232) {
    const int n = i - 16, level[6] = {0, 95, 135, 175, 215, 255};
    return QColor(level[n / 36], level[(n / 6) % 6], level[n % 6]);
  }
  const int g = 8 + (i - 232) * 10;
  return QColor(g, g, g);
}

VTermModifier vtermMods(Qt::KeyboardModifiers m) {
  int v = VTERM_MOD_NONE;
  if (m & Qt::ShiftModifier) v |= VTERM_MOD_SHIFT;
  if (m & Qt::AltModifier) v |= VTERM_MOD_ALT;
  if (m & Qt::ControlModifier) v |= VTERM_MOD_CTRL;
  return static_cast<VTermModifier>(v);
}

VTermKey vtermKey(int qtKey) {
  switch (qtKey) {
    case Qt::Key_Return: case Qt::Key_Enter: return VTERM_KEY_ENTER;
    case Qt::Key_Tab: case Qt::Key_Backtab: return VTERM_KEY_TAB;
    case Qt::Key_Backspace: return VTERM_KEY_BACKSPACE;
    case Qt::Key_Escape: return VTERM_KEY_ESCAPE;
    case Qt::Key_Up: return VTERM_KEY_UP;
    case Qt::Key_Down: return VTERM_KEY_DOWN;
    case Qt::Key_Left: return VTERM_KEY_LEFT;
    case Qt::Key_Right: return VTERM_KEY_RIGHT;
    case Qt::Key_Insert: return VTERM_KEY_INS;
    case Qt::Key_Delete: return VTERM_KEY_DEL;
    case Qt::Key_Home: return VTERM_KEY_HOME;
    case Qt::Key_End: return VTERM_KEY_END;
    case Qt::Key_PageUp: return VTERM_KEY_PAGEUP;
    case Qt::Key_PageDown: return VTERM_KEY_PAGEDOWN;
    default: break;
  }
  if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F12)
    return static_cast<VTermKey>(VTERM_KEY_FUNCTION(qtKey - Qt::Key_F1 + 1));
  return VTERM_KEY_NONE;
}

bool blankCell(const VTermScreenCell &c) {
  return c.chars[0] == 0 && VTERM_COLOR_IS_DEFAULT_BG(&c.bg) && !c.attrs.reverse;
}

}  // namespace

TermScreen::TermScreen(int rows, int cols, QObject *parent) : QObject(parent) {
  vt_ = vterm_new(std::max(rows, 1), std::max(cols, 1));
  vterm_set_utf8(vt_, 1);
  vterm_output_set_callback(vt_, &TermScreen::onOutput, this);
  state_ = vterm_obtain_state(vt_);
  screen_ = vterm_obtain_screen(vt_);
  static const VTermScreenCallbacks callbacks = {
      &TermScreen::onDamage, nullptr, nullptr, &TermScreen::onSetTermProp, &TermScreen::onBell,
      nullptr, &TermScreen::onPushLine, &TermScreen::onPopLine, &TermScreen::onSbClear};
  vterm_screen_set_callbacks(screen_, &callbacks, this);
  vterm_screen_enable_altscreen(screen_, 1);
  vterm_screen_set_damage_merge(screen_, VTERM_DAMAGE_ROW);
  vterm_screen_reset(screen_, 1);
}

TermScreen::~TermScreen() { vterm_free(vt_); }

void TermScreen::feed(const QByteArray &bytes) {
  pushed_ = false;
  vterm_input_write(vt_, bytes.constData(), static_cast<size_t>(bytes.size()));
  vterm_screen_flush_damage(screen_);
  if (pushed_) emit scrolled();
}

int TermScreen::rows() const {
  int r = 0, c = 0;
  vterm_get_size(vt_, &r, &c);
  return r;
}

int TermScreen::cols() const {
  int r = 0, c = 0;
  vterm_get_size(vt_, &r, &c);
  return c;
}

void TermScreen::resize(int rows, int cols) {
  pushed_ = false;
  vterm_set_size(vt_, std::max(rows, 1), std::max(cols, 1));
  vterm_screen_flush_damage(screen_);
  if (pushed_) emit scrolled();
  emit damaged(0, rows - 1);
}

int TermScreen::scrollback() const { return static_cast<int>(history_.size()); }

QColor TermScreen::colour(const VTermColor &in) const {
  if (VTERM_COLOR_IS_DEFAULT_FG(&in) || VTERM_COLOR_IS_DEFAULT_BG(&in)) return QColor();
  if (VTERM_COLOR_IS_INDEXED(&in)) return indexed(in.indexed.idx);
  return QColor(in.rgb.red, in.rgb.green, in.rgb.blue);
}

TermCell TermScreen::convert(const VTermScreenCell &c) const {
  TermCell out;
  if (c.chars[0] == static_cast<uint32_t>(-1)) {  // the second half of a wide character
    out.width = 0;
  } else {
    int n = 0;
    while (n < VTERM_MAX_CHARS_PER_CELL && c.chars[n] != 0) ++n;
    out.text = QString::fromUcs4(reinterpret_cast<const char32_t *>(c.chars), n);
    out.width = c.width == 2 ? 2 : 1;
  }
  out.fg = colour(c.fg);
  out.bg = colour(c.bg);
  out.bold = c.attrs.bold;
  out.italic = c.attrs.italic;
  out.underline = c.attrs.underline != 0;
  out.reverse = c.attrs.reverse;
  return out;
}

VTermScreenCell TermScreen::blank() const {
  VTermScreenCell c;
  std::memset(&c, 0, sizeof c);
  c.width = 1;
  vterm_state_get_default_colors(state_, &c.fg, &c.bg);
  return c;
}

TermCell TermScreen::cell(int row, int col) const {
  if (col < 0 || col >= cols()) return TermCell();
  if (row < 0) {
    const int back = -row;  // 1 is the newest
    if (back > static_cast<int>(history_.size())) return TermCell();
    const Line &line = history_[history_.size() - static_cast<size_t>(back)];
    if (static_cast<size_t>(col) >= line.size()) return TermCell();
    return convert(line[static_cast<size_t>(col)]);
  }
  if (row >= rows()) return TermCell();
  VTermScreenCell c;
  if (!vterm_screen_get_cell(screen_, VTermPos{row, col}, &c)) return TermCell();
  return convert(c);
}

QString TermScreen::text(int row) const {
  QString s;
  const int n = cols();
  for (int col = 0; col < n; ++col) {
    const TermCell c = cell(row, col);
    if (c.width == 0) continue;
    s += c.text.isEmpty() ? QStringLiteral(" ") : c.text;
  }
  int end = static_cast<int>(s.size());
  while (end > 0 && s[end - 1] == ' ') --end;
  s.truncate(end);
  return s;
}

QPoint TermScreen::cursor() const {
  VTermPos p;
  vterm_state_get_cursorpos(state_, &p);
  return QPoint(p.col, p.row);
}

bool TermScreen::cursorVisible() const { return cursorVisible_; }
bool TermScreen::altScreen() const { return altScreen_; }
bool TermScreen::mouseWanted() const { return mouse_; }
QString TermScreen::title() const { return title_; }

void TermScreen::key(int qtKey, Qt::KeyboardModifiers mods, const QString &text) {
  VTermModifier m = vtermMods(mods);
  if (qtKey == Qt::Key_Backtab) m = static_cast<VTermModifier>(m | VTERM_MOD_SHIFT);
  const VTermKey special = vtermKey(qtKey);
  if (special != VTERM_KEY_NONE) {
    vterm_keyboard_key(vt_, special, m);
    return;
  }
  // a printable character is sent with Alt and Control only: Shift is already in the character
  const VTermModifier cm = static_cast<VTermModifier>(m & ~VTERM_MOD_SHIFT);
  if ((m & VTERM_MOD_CTRL) && qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) {
    vterm_keyboard_unichar(vt_, static_cast<uint32_t>('a' + (qtKey - Qt::Key_A)), cm);
    return;
  }
  bool sent = false;
  for (const char32_t c : text.toUcs4()) {
    if (c < 0x20 || c == 0x7f) continue;  // Qt's text for a control key: the key code says it better
    vterm_keyboard_unichar(vt_, c, cm);
    sent = true;
  }
  if (!sent && qtKey >= Qt::Key_Space && qtKey <= Qt::Key_AsciiTilde) {
    uint32_t c = static_cast<uint32_t>(qtKey);
    if (c >= 'A' && c <= 'Z' && !(m & VTERM_MOD_SHIFT)) c += 'a' - 'A';
    vterm_keyboard_unichar(vt_, c, cm);
  }
}

void TermScreen::paste(const QString &text) {
  QByteArray bytes = text.toUtf8();
  bytes.replace("\r\n", "\r");
  bytes.replace('\n', '\r');
  vterm_keyboard_start_paste(vt_);  // writes the opening mark only when the program asked for it
  emit reply(bytes);
  vterm_keyboard_end_paste(vt_);
}

void TermScreen::mouse(int row, int col, Qt::MouseButton button, bool pressed,
                       Qt::KeyboardModifiers mods) {
  // libvterm counts a press it was not asked to report as held, and then reports no press at all
  if (!mouse_) return;
  int b = 0;
  switch (button) {
    case Qt::LeftButton: b = 1; break;
    case Qt::MiddleButton: b = 2; break;
    case Qt::RightButton: b = 3; break;
    default: return;
  }
  const VTermModifier m = vtermMods(mods);
  vterm_mouse_move(vt_, row, col, m);
  vterm_mouse_button(vt_, b, pressed, m);
}

void TermScreen::wheel(int row, int col, int steps) {
  if (!mouse_ || steps == 0) return;
  vterm_mouse_move(vt_, row, col, VTERM_MOD_NONE);
  for (int i = 0; i < std::abs(steps); ++i) vterm_mouse_button(vt_, steps > 0 ? 4 : 5, true, VTERM_MOD_NONE);
}

int TermScreen::onDamage(VTermRect rect, void *self) {
  emit static_cast<TermScreen *>(self)->damaged(rect.start_row, rect.end_row - 1);
  return 1;
}

int TermScreen::onSetTermProp(VTermProp prop, VTermValue *val, void *selfp) {
  auto *self = static_cast<TermScreen *>(selfp);
  switch (prop) {
    case VTERM_PROP_CURSORVISIBLE:
      self->cursorVisible_ = val->boolean;
      emit self->damaged(self->cursor().y(), self->cursor().y());
      break;
    case VTERM_PROP_ALTSCREEN: self->altScreen_ = val->boolean; break;
    case VTERM_PROP_MOUSE: self->mouse_ = val->number != VTERM_PROP_MOUSE_NONE; break;
    case VTERM_PROP_TITLE: {
      if (val->string.initial) self->titleBuf_.clear();
      self->titleBuf_ += QString::fromUtf8(val->string.str, static_cast<qsizetype>(val->string.len));
      if (val->string.final) {
        self->title_ = self->titleBuf_;
        emit self->titleChanged();
      }
      break;
    }
    default: break;
  }
  return 1;
}

int TermScreen::onBell(void *self) {
  emit static_cast<TermScreen *>(self)->bell();
  return 1;
}

int TermScreen::onPushLine(int cols, const VTermScreenCell *cells, void *selfp) {
  auto *self = static_cast<TermScreen *>(selfp);
  int used = cols;
  while (used > 0 && blankCell(cells[used - 1])) --used;
  self->history_.emplace_back(cells, cells + used);
  if (static_cast<int>(self->history_.size()) > kScrollbackLimit) self->history_.pop_front();
  self->pushed_ = true;
  return 1;
}

int TermScreen::onPopLine(int cols, VTermScreenCell *cells, void *selfp) {
  auto *self = static_cast<TermScreen *>(selfp);
  if (self->history_.empty()) return 0;
  const Line line = std::move(self->history_.back());
  self->history_.pop_back();
  const VTermScreenCell empty = self->blank();
  for (int i = 0; i < cols; ++i) cells[i] = static_cast<size_t>(i) < line.size() ? line[static_cast<size_t>(i)] : empty;
  return 1;
}

int TermScreen::onSbClear(void *selfp) {
  auto *self = static_cast<TermScreen *>(selfp);
  self->history_.clear();
  emit self->scrolled();
  return 1;
}

void TermScreen::onOutput(const char *s, size_t len, void *self) {
  emit static_cast<TermScreen *>(self)->reply(QByteArray(s, static_cast<qsizetype>(len)));
}
