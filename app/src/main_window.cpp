#include "main_window.hpp"

#include <QLabel>
#include <QSplitter>
#include <QVBoxLayout>

#include "board.hpp"

MainWindow::MainWindow(Board *board) : board_(board) {
  setWindowTitle("moth");
  resize(560, 860);

  auto *split = new QSplitter(this);
  auto *columnWidget = new QWidget(split);
  auto *outer = new QVBoxLayout(columnWidget);
  column_ = new QVBoxLayout;
  outer->addLayout(column_, 1);
  line_ = new QLabel(columnWidget);
  line_->setObjectName("line");
  line_->setTextFormat(Qt::PlainText);  // the core's words as they are: `<bottle>` is not markup
  line_->setWordWrap(true);
  outer->addWidget(line_);
  split->addWidget(columnWidget);
  setCentralWidget(split);

  connect(board, &Board::changed, this, [this] { showLine(); });
  poll_.setInterval(1000);
  connect(&poll_, &QTimer::timeout, board, &Board::tick);
  showLine();
}

QVBoxLayout *MainWindow::column() { return column_; }

void MainWindow::setPolling(bool on) {
  polling_ = on;
  if (!on) poll_.stop();
  else if (isVisible() && !poll_.isActive()) {
    poll_.start();
    board_->tick();
  }
}

void MainWindow::showEvent(QShowEvent *e) {
  QMainWindow::showEvent(e);
  if (polling_ && !poll_.isActive()) {
    poll_.start();
    board_->tick();
  }
}

void MainWindow::hideEvent(QHideEvent *e) {
  QMainWindow::hideEvent(e);
  poll_.stop();
}

void MainWindow::showLine() {
  line_->setText(board_->working() ? "working: " + board_->what() + QString::fromUtf8("…") : board_->line());
}
