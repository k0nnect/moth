#pragma once

#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <functional>

#include "core_client.hpp"

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
  // Starts the program in the bottle, then waits for it to be listed there and attaches to it.
  // The wait ends when the board shows that program attached (letter case aside), or after the
  // limit, when the line points at launch.log
  void launch(const QString &bottle, const QString &path);
  void detach();
  // the menu is built when it is about to show; each bottle's submenu likewise, looking in one
  // bottle at a time while the line says `looking in <bottle>…`. These are the slots that do it,
  // for tests that must not exec() a menu
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
  bool arrived() const;          // the program waited for is attached
  QString waitingLine() const;
  bool readPs(const QString &bottle, std::function<void(const Reply &)> then);
  void looked(QMenu *submenu, const QString &bottle, const Reply &r);
  void showProcesses(QMenu *submenu, const QString &bottle, const Reply &r);
  void fillTop(const Reply &bottles);
  void fillGames(const Reply &games);
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
  QAction *separator_ = nullptr;   // in the menu, between the bottles and the remembered games
  QString looking_;                // the bottle the menu's `ps` is out for, or ""
  QString lineBefore_;             // the line before it said `looking in <bottle>…`
  QPointer<QMenu> next_;           // a submenu opened while that ps was out, and its bottle
  QString nextBottle_;
  QHash<QString, QList<std::function<void(const Reply &)>>> psOut_;  // the bar's `ps` reads out, by bottle
};
