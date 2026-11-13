#pragma once

#include <QWidget>

class Board;
class QCheckBox;
class QLabel;
class QPushButton;
class QTableView;
class ResultsModel;

// The third panel: the hunt's results in a table named `results`, whose values can be typed over;
// `show rarer forms`; `keep` for the selected row; and a label named `shown` that says how much
// of the list is shown. On each tick the rows in view are read again.
class ResultsPanel : public QWidget {
  Q_OBJECT
public:
  explicit ResultsPanel(Board *board, QWidget *parent = nullptr);
  ResultsModel *model() const;
  // keeps the selected row, by the name given ("" lets the core name it)
  void keepSelected(const QString &name);

private:
  void askForName();     // thin: a small dialog for the optional name, then keepSelected
  void showState();
  QString shownText() const;
  void refreshInView();

  Board *board_;
  ResultsModel *model_;
  QTableView *view_;
  QLabel *shown_;
  QCheckBox *all_;
  QPushButton *keep_;
};
