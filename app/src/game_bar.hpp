#pragma once

#include <QTimer>
#include <QWidget>

class Board;
class QLabel;
class QMenu;
class QPushButton;

// The first panel: which game moth is attached to, a menu to pick a running game or launch one,
// and detach. What it shows follows Board::attached() and Board::changed, never its own notes.
class GameBar : public QWidget {
  Q_OBJECT
public:
  explicit GameBar(Board *board, QWidget *parent = nullptr);
  void attach(const QString &bottle, const QString &name);
  // starts the program in the bottle, then waits for it to be listed and attaches to it
  void launch(const QString &bottle, const QString &path);
  void detach();
  // the menu is built when it is about to show; each bottle's submenu likewise. These are the
  // slots that do it, for tests that must not exec() a menu
  QMenu *menu() const;
  void fillMenu();
  void fillBottle(QMenu *submenu, const QString &bottle);
  // test only: how often the waiting for a launched game looks, and when it gives up
  void setWaiting(int stepMs, int limitMs);

private:
  void showState();
  void startWaiting(const QString &bottle, const QString &name);
  void stopWaiting();
  void look();
  void askForAnotherGame();   // thin: a file dialog and a bottle list, then launch

  Board *board_;
  QLabel *label_;
  QPushButton *pick_, *detach_;
  QMenu *menu_;
  QTimer step_;
  int stepMs_ = 2000, limitMs_ = 90000;
  int waited_ = 0;
  int generation_ = 0;        // bumps whenever a wait ends or starts, so a late answer is ignored
  bool waiting_ = false;
  QString waitBottle_, waitName_;
  int menuGeneration_ = 0;
};
