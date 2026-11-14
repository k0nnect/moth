#include "search_bar.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>
#include <utility>

#include "board.hpp"

SearchBar::SearchBar(Board *board, QWidget *parent) : QWidget(parent), board_(board) {
  setObjectName("searchBar");
  auto *column = new QVBoxLayout(this);
  auto *first = new QHBoxLayout;
  number_ = new QLineEdit(this);
  number_->setObjectName("number");
  number_->setPlaceholderText("the number the game shows");
  hunt_ = new QPushButton("new hunt", this);
  hunt_->setObjectName("hunt");
  narrow_ = new QPushButton("narrow", this);
  narrow_->setObjectName("narrow");
  snapshot_ = new QPushButton("no number", this);
  snapshot_->setObjectName("snapshot");
  first->addWidget(number_, 1);
  first->addWidget(hunt_);
  first->addWidget(narrow_);
  first->addWidget(snapshot_);
  column->addLayout(first);

  auto *second = new QHBoxLayout;
  const QList<QPair<QString, QString>> relations = {{"down", "went down"}, {"up", "went up"}, {"same", "same"}, {"changed", "changed"}};
  for (const auto &[how, text] : relations) {
    auto *b = new QPushButton(text, this);
    b->setObjectName(how);
    connect(b, &QPushButton::clicked, this, [this, how] { compare(how); });
    second->addWidget(b);
    relations_.append(b);
  }
  second->addStretch(1);
  column->addLayout(second);

  counts_ = new QLabel(this);
  counts_->setObjectName("counts");
  counts_->setTextFormat(Qt::PlainText);
  counts_->setWordWrap(true);
  column->addWidget(counts_);

  connect(number_, &QLineEdit::returnPressed, this, &SearchBar::entered);
  connect(hunt_, &QPushButton::clicked, this, &SearchBar::find);
  connect(narrow_, &QPushButton::clicked, this, &SearchBar::narrow);
  connect(snapshot_, &QPushButton::clicked, this, &SearchBar::snapshot);
  connect(board, &Board::changed, this, &SearchBar::showState);
  showState();
}

void SearchBar::showState() {
  const bool free = board_->attached().has_value() && !board_->working();
  const auto &hunt = board_->hunt();
  number_->setEnabled(free);
  hunt_->setEnabled(free);
  snapshot_->setEnabled(free);
  narrow_->setEnabled(free && hunt.has_value());
  for (QPushButton *b : relations_) b->setEnabled(free && hunt && hunt->canCompare);
  counts_->setText(board_->counts());
  // a disabled box loses its focus and its selection: both come back once it can be typed in
  if (free && std::exchange(selectNext_, false)) {
    number_->setFocus();
    number_->selectAll();
  }
}

QString SearchBar::typed() {
  const QString n = number_->text().trimmed();
  if (n.isEmpty()) board_->say("type the number the game shows");
  return n;
}

void SearchBar::entered() {
  if (board_->hunt()) narrow();
  else find();
}

void SearchBar::find() {
  const QString n = typed();
  if (!n.isEmpty()) step("find", {n}, true);
}

void SearchBar::narrow() {
  const QString n = typed();
  if (!n.isEmpty()) step("next", {n}, true);
}

void SearchBar::snapshot() { step("snapshot", {}, true); }

void SearchBar::compare(const QString &how) { step("next", {how}, false); }

// a second press before the first step is answered sends nothing: Board::run refuses it and the
// line says what is running. A step the box started (`fromBox`) empties it once it worked; the
// four relation buttons leave it alone
void SearchBar::step(const QString &cmd, const QStringList &args, bool fromBox) {
  board_->run(cmd, args, {}, [self = QPointer<SearchBar>(this), fromBox](const Reply &r) {
    if (!self) return;
    if (fromBox && r.ok) self->number_->clear();
    self->selectNext_ = r.ok;
    self->showState();
  });
}
