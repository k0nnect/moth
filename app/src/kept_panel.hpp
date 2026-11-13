#pragma once

#include <QWidget>

class Board;
class KeptModel;
class KeptTable;
class QLabel;
class QPushButton;

// The last panel: the values the user has kept, in a table named `kept` (name, value, kind, note)
// read from `kept` after each event and on each tick. A value can be typed over (`set`), the
// selected one forgotten (`forget`), and the last change undone (`undo`); the core's answers
// reach the window's line through Board::run. With no game the table is empty and nothing is
// read; with a game and nothing kept, a label says how to keep something.
class KeptPanel : public QWidget {
  Q_OBJECT
public:
  explicit KeptPanel(Board *board, QWidget *parent = nullptr);

private:
  void showState();
  void forgetSelected();

  Board *board_;
  KeptModel *model_;
  KeptTable *view_;
  QLabel *nothing_;
  QPushButton *forget_, *undo_;
  bool loaded_ = false;       // a `kept` reply has come for the game attached now
  bool wasAttached_ = false;
};
