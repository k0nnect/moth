#pragma once

#include <QMainWindow>
#include <QTimer>

class Board;
class QLabel;
class QVBoxLayout;

// The window: one column of panels in a splitter (stage 2b adds the terminal beside it), and under
// the column a label named `line` with what the board has to say, or what is running.
class MainWindow : public QMainWindow {
public:
  explicit MainWindow(Board *board);
  QVBoxLayout *column();      // where the panels go, top to bottom
  // on (the default): while the window is visible, Board::tick() runs on showing and then every
  // second. off: never; for tests that count requests
  void setPolling(bool on);

protected:
  void showEvent(QShowEvent *e) override;
  void hideEvent(QHideEvent *e) override;

private:
  void showLine();

  Board *board_;
  QVBoxLayout *column_;
  QLabel *line_;
  QTimer poll_;
  bool polling_ = true;
};
