// The real window against the real core and a real game, for the end-to-end scenarios 33 to 36,
// 39, 40 and 41 (core/integration/it_main.ml), which run it. Not a ctest test.
//
//   it_app <edit|follow|gone> <bottle> <exe name> <number> <0xaddr> <new number>
//   it_app term <bottle> <exe name> <number> <text>
//
// MOTH_HOME and MOTH_BIN come from the environment (it refuses to run without them, so it never
// touches the default home), and the window is offscreen. It attaches from the game bar, types
// <number> and Return, and finds the result at <0xaddr> that is a whole number, reading more of
// the list as needed (the 20,000 rows the list holds at most); then it prints `listed`.
//   edit:   types <new number> over that row's value in the cell's own editor and commits it
//           with Return, keeps the row as `points`, and waits for the kept table to show `points`
//           and <new number>. After each of the two writes it waits for the list's check after
//           the write's event to come back before it looks for a reset.
//   follow: changes nothing itself, and waits up to 30 seconds for an event from another client,
//           the list read again after it, and the row at <0xaddr> to show <new number>.
//   gone:   changes nothing itself, and waits up to 30 seconds for the game to be gone (someone
//           else ends it): the line says the core's words for a closed game, the results list and
//           the kept table have no rows, the game bar says `no game attached`, and the search box
//           can't be used. Then it asks the search bar to find <number>, which the core must refuse
//           as not attached. <new number> is not used.
//   term:   the window with its terminal on (the harness sets MOTH_CLAUDE, SHELL and HOME so that no
//           real agent or shell start-up file runs; it refuses to run without MOTH_CLAUDE, so run
//           by hand it never starts the real agent). It waits up to 60 seconds for <text> on the
//           terminal's screen and prints `terminal`; attaches, types <number> and Return in the
//           search bar as the others do, and prints `clicked`. Then it takes commands, one per line,
//           from its stdin, and ends (closing the window) at the first that ends it, or at the end
//           of stdin:
//             quiet  ticks the `quiet` box, waits for the board to say so, prints `quiet`
//             loud   unticks it, waits for the board to say so, prints `loud`
//             bye    types `bye` and Return into the terminal, then does what `shell` does
//             shell  waits for a shell prompt (a row ending in `$`), types `moth status` and Return,
//                    waits for `attached to` on the screen, prints `shell`, and ends
//             close  ends at once, with whatever runs in the terminal still running
// It prints one line per step as it goes, then `ok`, exit 0. A step that does not come true in
// time prints what it waited for and the window's line, exit 1. Whatever happens, it ends by
// itself after three minutes.

#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QTimer>
#include <poll.h>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <utility>
#include <unistd.h>

#include "board.hpp"
#include "core_client.hpp"
#include "game_bar.hpp"
#include "kept_panel.hpp"
#include "main_window.hpp"
#include "paths.hpp"
#include "results_model.hpp"
#include "results_panel.hpp"
#include "search_bar.hpp"
#include "term_screen.hpp"
#include "terminal_view.hpp"

namespace {

constexpr unsigned overallSeconds = 180;
const QString dot = QString::fromUtf8(" · ");

void print(const QString &line) {
  std::fputs(line.toUtf8().constData(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

// the overall deadline: async-signal-safe, so it works even if the event loop is stuck
void overran(int) {
  static const char said[] = "failed: it_app ran past its three minutes\n";
  ssize_t ignored = write(STDOUT_FILENO, said, sizeof said - 1);
  (void)ignored;
  _exit(1);
}

// a 0x address as a number; 0 when it isn't one
quint64 addressOf(const QString &text) {
  bool ok = false;
  const quint64 a = text.toULongLong(&ok, 0);
  return ok ? a : 0;
}

class Drive {
public:
  Drive(MainWindow &window, Board &board) : board_(board) {
    line_ = need<QLabel>(&window, "line");
    gameBar_ = need<GameBar>(&window, "gameBar");
    gameLabel_ = need<QLabel>(gameBar_, "game");
    searchBar_ = need<SearchBar>(&window, "searchBar");
    number_ = need<QLineEdit>(searchBar_, "number");
    hunt_ = need<QPushButton>(searchBar_, "hunt");
    panel_ = need<ResultsPanel>(&window, "resultsPanel");
    model_ = panel_->model();
    results_ = need<QTableView>(panel_, "results");
    all_ = need<QCheckBox>(panel_, "all");
    kept_ = need<QTableView>(need<KeptPanel>(&window, "keptPanel"), "kept");
    QObject::connect(&board, &Board::turned, &context_, [this] {
      ++turns_;
      readSinceTurn_ = 0;
    });
    QObject::connect(model_, &QAbstractItemModel::modelReset, &context_, [this] { ++resets_; });
    // listChanged comes when a write is answered (writing() turns false), and when anything else
    // about the list changes, such as a page read coming back
    QObject::connect(model_, &ResultsModel::listChanged, &context_, [this] {
      const bool writing = model_->writing();
      if (std::exchange(writing_, writing) == writing) ++readSinceTurn_;  // not a write starting or ending
    });
  }

  // the window's terminal, which the term mode needs
  void useTerminal(MainWindow &window) {
    terminal_ = need<TerminalView>(&window, "terminal");
    quietBox_ = need<QCheckBox>(&window, "quiet");
  }

  [[noreturn]] void fail(const QString &why) {
    print("failed: " + why + ". the window's line: " + (line_ ? line_->text() : QString()));
    std::_Exit(1);
  }

  // runs the window until `done` is true; past `seconds`, fails saying what it waited for
  void waitFor(const QString &what, int seconds, const std::function<bool()> &done) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
      if (clock.elapsed() >= seconds * 1000) fail(what + ": not within " + QString::number(seconds) + " seconds");
      QEventLoop loop;
      QTimer::singleShot(10, &loop, &QEventLoop::quit);
      loop.exec();
    }
  }

  void attach(const QString &bottle, const QString &exe) {
    waitFor("moth's background service to answer", 30, [this] { return board_.connected() && !board_.working(); });
    print("the service answers");
    gameBar_->attach(bottle, exe);
    print("attach " + exe + " in " + bottle);
    const QString shows = exe + dot + bottle + dot;
    waitFor("the game bar to show " + shows + "...", 90, [&] { return gameLabel_->text().startsWith(shows, Qt::CaseInsensitive); });
    print("the game bar shows " + gameLabel_->text());
  }

  // the number typed into the search box, then Return: with no hunt, a find
  void search(const QString &number) {
    waitFor("the search box to take a number", 30, [this] { return number_->isEnabled(); });
    if (board_.hunt()) fail("a hunt is already under way before the search");
    number_->setText(number);
    const int turns = turns_;
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(number_, &press);
    QApplication::sendEvent(number_, &release);
    print("typed " + number + ", Return");
    waitFor("find " + number + " to end, with a hunt", 120,
            [&] { return turns_ > turns && board_.hunt() && !board_.working() && number_->isEnabled(); });
    print("the hunt: " + board_.counts().section('\n', 0, 0));
    waitFor("the results list to show rows", 60, [this] { return model_->listed() && model_->rowCount() > 0 && model_->total() > 0; });
    print("results: " + QString::number(model_->rowCount()) + " rows read of " + QString::number(model_->total()));
    if (all_->isChecked()) all_->setChecked(false);
    if (model_->all()) fail("show rarer forms is still on");
    print("show rarer forms is off");
  }

  // the row of the list showing a whole number at `addr`, or -1
  int rowAt(quint64 addr) const {
    for (int r = 0; r < model_->rowCount(); ++r)
      if (addressOf(cell(r, ResultsModel::Address)) == addr && cell(r, ResultsModel::Kind) == "whole number") return r;
    return -1;
  }

  // the row at `addr` as far as the list is read, reading the next page when there is one
  int look(quint64 addr) {
    const int r = rowAt(addr);
    if (r < 0 && model_->canFetchMore({})) model_->fetchMore({});
    return r;
  }

  // every row the list will hold is read: all of them, or the cap
  bool readToTheEnd() const {
    return model_->listed() && !model_->canFetchMore({}) &&
           model_->rowCount() >= std::min<qint64>(model_->total(), ResultsModel::cap);
  }

  // finds the row at `addr`, selects it and shows it, as a click on it would
  int pick(const QString &address) {
    const quint64 addr = addressOf(address);
    int row = -1;
    waitFor("a whole number at " + address + " in the list", 120, [&] {
      row = look(addr);
      return row >= 0 || readToTheEnd();
    });
    if (row < 0)
      fail("no whole number at " + address + " among the " + QString::number(model_->rowCount()) + " rows read of " +
           QString::number(model_->total()));
    const QModelIndex at = model_->index(row, ResultsModel::Value);
    results_->selectionModel()->setCurrentIndex(at, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    results_->scrollTo(at);
    print("row " + cell(row, ResultsModel::Number) + " is at " + address + ", showing " + cell(row, ResultsModel::Value));
    return row;
  }

  // types `value` over the row's value in the cell's own editor, commits it with Return, then
  // keeps the row as `name`
  void edit(int row, const QString &address, const QString &value, const QString &name) {
    const quint64 addr = addressOf(address);
    waitFor("moth to be free for a write", 30, [this] { return !board_.working() && !model_->writing(); });
    const int turns = turns_, resets = resets_;
    const QModelIndex at = model_->index(row, ResultsModel::Value);
    results_->setCurrentIndex(at);
    results_->edit(at);
    auto *editor = results_->viewport()->findChild<QLineEdit *>();
    if (!editor || !editor->isVisible()) fail("no editor opened on row " + cell(row, ResultsModel::Number) + "'s value");
    print("the editor on row " + cell(row, ResultsModel::Number) + " shows " + editor->text());
    editor->setText(value);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(editor, &press);
    QApplication::sendEvent(editor, &release);
    print("typed " + value + " over row " + cell(row, ResultsModel::Number) + ", Return");
    waitFor("the write of " + value + " to be sent", 10, [&] { return model_->writing() || turns_ > turns; });
    // the write's event has come back, the row shows the value, and the list's check after that
    // event (R14) has come back too, so a reset it caused would have happened
    waitFor("row " + cell(row, ResultsModel::Number) + " to show " + value + " and the list to be checked after the write", 30, [&] {
      return turns_ > turns && readSinceTurn_ > 0 && !board_.working() && !model_->writing() && row < model_->rowCount() &&
             cell(row, ResultsModel::Value) == value;
    });
    stillSelected(row, addr, resets);
    print("row " + cell(row, ResultsModel::Number) + " shows " + value + ", still selected");
    const int keptTurns = turns_;
    panel_->keepSelected(name);
    print("keep it as " + name);
    waitFor("the kept table to show " + name + " " + value + " and the list to be checked after the keep", 30, [&] {
      return keptShows(name, value) && turns_ > keptTurns && readSinceTurn_ > 0 && !model_->writing();
    });
    print("kept: " + name + " " + value);
    stillSelected(row, addr, resets);
  }

  // waits for an event from another client, the list read again after it and, read as far as
  // needed, `value` at `address`
  void follow(const QString &address, const QString &value) {
    const quint64 addr = addressOf(address);
    const int turns = turns_;
    int row = -1;
    waitFor("an event, the list read again after it, and " + value + " at " + address, 30, [&] {
      if (turns_ == turns || readSinceTurn_ == 0) return false;
      row = look(addr);
      return row >= 0 && cell(row, ResultsModel::Value) == value;
    });
    print("the list read itself again: row " + cell(row, ResultsModel::Number) + " is at " + address + ", showing " + value +
          ", of " + QString::number(model_->total()));
  }

  // waits for the game to be gone, as the window shows it, then asks for a find, which the core
  // must refuse: with no game nothing can be written
  void gone(const QString &exe, const QString &number) {
    const QString closed = "not attached. " + exe + " closed.", lost = "the game closed. attach again once it's running.";
    waitFor("the line to say " + exe + " closed, with no rows listed or kept and no game in the bar", 30, [&] {
      const QString line = line_->text();
      return (line.compare(closed, Qt::CaseInsensitive) == 0 || line == lost) && model_->rowCount() == 0 &&
             kept_->model()->rowCount() == 0 && gameLabel_->text() == "no game attached";
    });
    print("the window says: " + line_->text());
    print("no rows listed, none kept, and the game bar says " + gameLabel_->text());
    if (number_->isEnabled() || hunt_->isEnabled()) fail("the search box can still be used with no game");
    print("the search box can't be used");
    number_->setText(number);
    searchBar_->find();
    const QString refused = "not attached. run: moth attach --bottle <bottle> <game.exe>";
    waitFor("find " + number + " to be refused: " + refused, 30, [&] { return line_->text() == refused; });
    if (board_.hunt() || model_->rowCount() != 0 || kept_->model()->rowCount() != 0) fail("something came back after the find");
    print("find " + number + " was refused: " + line_->text());
  }


  bool screenShows(const QString &text) const {
    for (int r = 0; r < terminal_->screen()->rows(); ++r)
      if (terminal_->screen()->text(r).contains(text, Qt::CaseInsensitive)) return true;
    return false;
  }

  bool promptShown() const {
    for (int r = 0; r < terminal_->screen()->rows(); ++r)
      if (terminal_->screen()->text(r).endsWith('$')) return true;
    return false;
  }

  QString screenText() const {
    QStringList rows;
    for (int r = 0; r < terminal_->screen()->rows(); ++r) rows << terminal_->screen()->text(r);
    return rows.join(" | ");
  }

  // the terminal's screen shows `text` (the screen is put in the failure's words)
  void terminalShows(const QString &text, int seconds) {
    waitFor("the terminal to show \"" + text + "\". the screen: " + screenText(), seconds, [&] { return screenShows(text); });
    print("the terminal shows " + text);
  }

  // keys sent to the terminal one at a time, as typing does; \n is Return
  void type(const QString &text) {
    for (const QChar c : text) {
      int key = Qt::Key_unknown;
      QString sent = QString(c);
      if (c == '\n') {
        key = Qt::Key_Return;
        sent = "\r";
      } else if (c == ' ') key = Qt::Key_Space;
      else if (c >= 'a' && c <= 'z') key = Qt::Key_A + (c.unicode() - 'a');
      QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, sent);
      QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, sent);
      QApplication::sendEvent(terminal_, &press);
      QApplication::sendEvent(terminal_, &release);
    }
  }

  // one line from stdin; "" at its end
  QString command() {
    QString line;
    bool closed = false;
    waitFor("a line on stdin", overallSeconds, [&] {
      pollfd p{STDIN_FILENO, POLLIN, 0};
      if (poll(&p, 1, 0) <= 0) return false;
      char ch;
      const ssize_t n = read(STDIN_FILENO, &ch, 1);
      if (n <= 0) {
        closed = true;
        return true;
      }
      if (ch == '\n') return true;
      line += QChar(ch);
      return false;
    });
    return closed && line.isEmpty() ? QString() : line;
  }

  void tickQuiet() {
    waitFor("the quiet box to be usable", 30, [this] { return quietBox_->isEnabled(); });
    quietBox_->click();
    waitFor("the board to say quiet is on", 30, [this] { return board_.quiet() && !board_.working(); });
    if (!quietBox_->isChecked()) fail("the quiet box is not ticked");
    print("quiet");
  }

  void untickQuiet() {
    waitFor("the quiet box to be usable", 30, [this] { return quietBox_->isEnabled(); });
    quietBox_->click();
    waitFor("the board to say quiet is off", 30, [this] { return !board_.quiet() && !board_.working(); });
    if (quietBox_->isChecked()) fail("the quiet box is still ticked");
    print("loud");
  }

  void shell() {
    waitFor("a shell prompt on the terminal. the screen: " + screenText(), 30, [this] { return promptShown(); });
    type("moth status\n");
    terminalShows("attached to", 30);
    print("shell");
  }

  // the term mode, after the search: what the harness tells it
  void commands() {
    for (;;) {
      const QString c = command();
      if (c == "quiet") tickQuiet();
      else if (c == "loud") untickQuiet();
      else if (c == "bye") {
        type("bye\n");
        shell();
        return;
      } else if (c == "shell") {
        shell();
        return;
      } else if (c == "close" || c.isEmpty()) return;
      else fail("it_app was told \"" + c + "\"");
    }
  }

private:
  template <typename W>
  W *need(QObject *in, const QString &name) {
    W *w = in->findChild<W *>(name);
    if (!w) fail("the window has no " + name);
    return w;
  }

  QString cell(int row, int column) const { return model_->data(model_->index(row, column)).toString(); }

  // the list was not read again from the start, and `row` (still at `addr`) is the one selected
  void stillSelected(int row, quint64 addr, int resets) {
    if (resets_ != resets) fail("the results list was read again from the start after the write");
    const QModelIndexList picked = results_->selectionModel()->selectedRows();
    if (picked.size() != 1 || picked.first().row() != row || rowAt(addr) != row) fail("the row written is no longer the one selected");
  }

  bool keptShows(const QString &name, const QString &value) const {
    const QAbstractItemModel *m = kept_->model();
    for (int r = 0; r < m->rowCount(); ++r)
      if (m->data(m->index(r, 0)).toString() == name && m->data(m->index(r, 1)).toString() == value) return true;
    return false;
  }

  QObject context_;  // the connections above end with the Drive
  Board &board_;
  QLabel *line_ = nullptr, *gameLabel_ = nullptr;
  GameBar *gameBar_ = nullptr;
  SearchBar *searchBar_ = nullptr;
  QLineEdit *number_ = nullptr;
  QPushButton *hunt_ = nullptr;
  ResultsPanel *panel_ = nullptr;
  ResultsModel *model_ = nullptr;
  QTableView *results_ = nullptr, *kept_ = nullptr;
  QCheckBox *all_ = nullptr, *quietBox_ = nullptr;
  TerminalView *terminal_ = nullptr;
  int turns_ = 0, resets_ = 0;
  int readSinceTurn_ = 0;  // list changes since the latest event's turn, other than a write starting or ending
  bool writing_ = false;   // the model's writing() as last seen
};

}  // namespace

int main(int argc, char **argv) {
  std::signal(SIGALRM, overran);
  alarm(overallSeconds);
  const QString usage =
      "usage: it_app <edit|follow|gone> <bottle> <exe name> <number> <0xaddr> <new number>\n       it_app term <bottle> <exe name> <number> <text>";
  const QStringList modes{"edit", "follow", "gone"};
  const bool term = argc == 6 && QString::fromUtf8(argv[1]) == "term";
  if (!term && (argc != 7 || !modes.contains(QString::fromUtf8(argv[1])))) {
    std::fprintf(stderr, "%s\n", usage.toUtf8().constData());
    return 2;
  }
  if (qEnvironmentVariableIsEmpty("MOTH_HOME") || qEnvironmentVariableIsEmpty("MOTH_BIN")) {
    std::fprintf(stderr, "it_app needs MOTH_HOME and MOTH_BIN: it never uses the default home\n");
    return 2;
  }
  if (term && qEnvironmentVariableIsEmpty("MOTH_CLAUDE")) {
    std::fprintf(stderr, "it_app term needs MOTH_CLAUDE, the stand-in agent: it never starts the real one\n");
    return 2;
  }
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");

  QApplication app(argc, argv);
  const QString mode = argv[1], bottle = argv[2], exe = argv[3], number = argv[4], fifth = argv[5], value = term ? QString() : argv[6];
  const QString address = term ? QString() : fifth;
  const QString waitText = term ? fifth : QString();
  if (!term && addressOf(address) == 0) {
    std::fprintf(stderr, "%s is not an address like 0x7ffe1234\n", argv[5]);
    return 2;
  }
  CoreClient core(mothHome(), mothBin());
  Board board(&core);
  MainWindow window(&board);
  if (!term) window.setTerminal(false);  // no shell, no agent: this drives the panels
  window.show();
  Drive drive(window, board);
  if (term) {
    drive.useTerminal(window);
    drive.terminalShows(waitText, 60);
    print("terminal");
    drive.attach(bottle, exe);
    drive.search(number);
    print("clicked");
    drive.commands();
    window.close();
    print("ok");
    return 0;
  }

  drive.attach(bottle, exe);
  drive.search(number);
  const int row = drive.pick(address);
  print("listed");
  if (mode == "edit") drive.edit(row, address, value, "points");
  else if (mode == "follow") drive.follow(address, value);
  else drive.gone(exe, number);
  print("ok");
  return 0;
}
