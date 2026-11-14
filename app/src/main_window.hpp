#pragma once

#include <QMainWindow>
#include <QTimer>

class Board;
class QCheckBox;
class QLabel;
class QSplitter;
class QVBoxLayout;
class QWidget;
class TerminalView;

// The window: a splitter with the column of panels on the left and, on the right, a strip (the
// terminal's title, a `quiet` checkbox) over a terminal. Under the column of panels is a label
// named `line` with what the board has to say, or what is running.
//
// The terminal starts, once, when the window is first shown: in the user's home folder, the
// user's shell running `moth agent` (MOTH_TERMINAL) and then a shell again, so that when the agent
// ends or cannot start a shell with `moth` on its PATH is left. When that shell ends the view keeps
// its last screen, the strip says so and Return in the view starts it again (Command+C and Command+A still copy and select, any other key does nothing). Closing the window
// destroys the terminal first, so its child and its process group are hung up on at once.
class MainWindow : public QMainWindow {
public:
  explicit MainWindow(Board *board);
  ~MainWindow() override;
  QVBoxLayout *column();      // where the panels go, top to bottom
  TerminalView *terminal();   // null with the terminal off, and once the window is closed
  // the terminal is there by default and starts when the window is first shown. setTerminal(false),
  // for tests of the panels alone, removes it for good: nothing is started, and the window is the
  // column's width. setTerminal(true) does nothing (a removed terminal is not brought back)
  void setTerminal(bool on);
  // on (the default): while the window is visible, Board::tick() runs on showing and then every
  // second. off: never; for tests that count requests
  void setPolling(bool on);

protected:
  void showEvent(QShowEvent *e) override;
  void hideEvent(QHideEvent *e) override;
  void closeEvent(QCloseEvent *e) override;
  bool eventFilter(QObject *watched, QEvent *e) override;

private:
  void showLine();
  void buildTerminal();
  void dropTerminal();
  void startTerminal();
  void showTitle();
  void syncQuiet();
  void quietClicked(bool on);

  Board *board_;
  QSplitter *split_;
  QWidget *columnWidget_;
  QVBoxLayout *column_;
  QLabel *line_;
  QWidget *pane_ = nullptr;   // the strip and the terminal
  QLabel *title_ = nullptr;
  QCheckBox *quiet_ = nullptr;
  TerminalView *terminal_ = nullptr;
  bool started_ = false;      // the terminal has been started once
  bool ended_ = false;        // its shell ended: Return starts it again
  QTimer poll_;
  bool polling_ = true;
};
