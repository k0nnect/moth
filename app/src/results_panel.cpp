#include "results_panel.hpp"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include "board.hpp"
#include "results_model.hpp"

namespace {

// 16700000 as 16,700,000, whatever the machine's locale
QString commas(qint64 n) {
  QString digits = QString::number(n < 0 ? -n : n);
  for (qsizetype i = digits.size() - 3; i > 0; i -= 3) digits.insert(i, ',');
  return n < 0 ? "-" + digits : digits;
}

}  // namespace

ResultsPanel::ResultsPanel(Board *board, QWidget *parent) : QWidget(parent), board_(board) {
  setObjectName("resultsPanel");
  model_ = new ResultsModel(board, this);

  auto *column = new QVBoxLayout(this);
  auto *top = new QHBoxLayout;
  shown_ = new QLabel(this);
  shown_->setObjectName("shown");
  shown_->setTextFormat(Qt::PlainText);
  shown_->setWordWrap(true);
  all_ = new QCheckBox("show rarer forms", this);
  all_->setObjectName("all");
  keep_ = new QPushButton("keep", this);
  keep_->setObjectName("keep");
  top->addWidget(shown_, 1);
  top->addWidget(all_);
  top->addWidget(keep_);
  column->addLayout(top);

  view_ = new QTableView(this);
  view_->setObjectName("results");
  view_->setModel(model_);
  view_->setSelectionBehavior(QAbstractItemView::SelectRows);
  view_->setSelectionMode(QAbstractItemView::SingleSelection);
  view_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed |
                         QAbstractItemView::AnyKeyPressed);
  view_->verticalHeader()->hide();
  view_->horizontalHeader()->setStretchLastSection(true);
  column->addWidget(view_, 1);

  connect(all_, &QCheckBox::toggled, model_, &ResultsModel::setAll);
  connect(keep_, &QPushButton::clicked, this, &ResultsPanel::askForName);
  connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ResultsPanel::showState);
  // a reset forgets the selection without saying so
  connect(model_, &QAbstractItemModel::modelReset, this, &ResultsPanel::showState);
  connect(model_, &ResultsModel::listChanged, this, &ResultsPanel::showState);
  connect(board, &Board::changed, this, &ResultsPanel::showState);
  connect(board, &Board::ticked, this, &ResultsPanel::refreshInView);
  showState();
}

ResultsModel *ResultsPanel::model() const { return model_; }

void ResultsPanel::showState() {
  keep_->setEnabled(view_->selectionModel()->hasSelection() && !board_->working() && !model_->writing());
  shown_->setText(shownText());
}

QString ResultsPanel::shownText() const {
  const auto &hunt = board_->hunt();
  if (!board_->attached() || !hunt) return {};
  if (hunt->blind) return "nothing to list yet";
  if (!model_->error().isEmpty()) return model_->error();
  if (!model_->listed()) return {};
  const qint64 total = model_->total(), rows = model_->rowCount();
  if (rows >= total) return commas(total) + (total == 1 ? " match" : " matches");
  if (rows >= ResultsModel::cap)
    return "showing the first " + commas(ResultsModel::cap) + " of " + commas(total) + ". narrow the hunt to see fewer.";
  return "showing " + commas(rows) + " of " + commas(total);
}

void ResultsPanel::refreshInView() {
  const int first = view_->rowAt(0);
  if (first < 0) return;
  int last = view_->rowAt(view_->viewport()->height() - 1);
  if (last < 0) last = model_->rowCount() - 1;
  model_->refresh(first, last);
}

void ResultsPanel::keepSelected(const QString &name) {
  const QModelIndexList picked = view_->selectionModel()->selectedRows();
  if (picked.isEmpty()) {
    board_->say("pick a row to keep");
    return;
  }
  model_->keep(picked.first().row(), name);
}

void ResultsPanel::askForName() {
  bool ok = false;
  const QString name = QInputDialog::getText(this, "keep", "a name for it (you can leave this empty)", QLineEdit::Normal, {}, &ok);
  if (ok) keepSelected(name.trimmed());
}
