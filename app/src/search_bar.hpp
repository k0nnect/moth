#pragma once

#include <QWidget>

class Board;
class QLabel;
class QLineEdit;
class QPushButton;

// The second panel: a box for the number the game shows, with no type to choose, and the steps of
// a hunt. Return narrows when a hunt is in progress and starts one otherwise. Everything is
// disabled with no game attached and while moth is working; a step pressed while the last is still
// unanswered is refused by Board::run. `narrow` needs a hunt, and the four relations a hunt that
// can be compared by change. What it shows follows Board::changed.
class SearchBar : public QWidget {
  Q_OBJECT
public:
  explicit SearchBar(Board *board, QWidget *parent = nullptr);
  void find();                         // `find <number>`
  void narrow();                       // `next <number>`
  void snapshot();                     // `snapshot`
  void compare(const QString &how);    // `next down`, `up`, `same` or `changed`

private:
  void entered();
  void step(const QString &cmd, const QStringList &args);
  QString typed();                     // trimmed; nothing typed: says so and is ""
  void showState();

  Board *board_;
  QLineEdit *number_;
  QPushButton *hunt_, *narrow_, *snapshot_;
  QList<QPushButton *> relations_;
  QLabel *counts_;
  bool selectNext_ = false;  // a step worked: the box's text is selected once it can be typed in
};
