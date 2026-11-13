#include "game_bar.hpp"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>

#include "board.hpp"

namespace {

const QString dot = QString::fromUtf8(" · ");
const QString ellipsis = QString::fromUtf8("…");

QString baseName(const QString &path) {
  return path.mid(qMax(path.lastIndexOf('/'), path.lastIndexOf('\\')) + 1);
}

}  // namespace

GameBar::GameBar(Board *board, QWidget *parent) : QWidget(parent), board_(board) {
  setObjectName("gameBar");
  auto *row = new QHBoxLayout(this);
  label_ = new QLabel(this);
  label_->setObjectName("game");
  label_->setTextFormat(Qt::PlainText);
  pick_ = new QPushButton("pick a game", this);
  pick_->setObjectName("pick");
  detach_ = new QPushButton("detach", this);
  detach_->setObjectName("detach");
  row->addWidget(label_, 1);
  row->addWidget(pick_);
  row->addWidget(detach_);

  menu_ = new QMenu(this);
  pick_->setMenu(menu_);
  connect(menu_, &QMenu::aboutToShow, this, &GameBar::fillMenu);
  connect(detach_, &QPushButton::clicked, this, &GameBar::detach);

  step_.setSingleShot(false);
  connect(&step_, &QTimer::timeout, this, &GameBar::look);
  connect(board, &Board::changed, this, &GameBar::showState);
  showState();
}

QMenu *GameBar::menu() const { return menu_; }

void GameBar::setWaiting(int stepMs, int limitMs) {
  stepMs_ = stepMs;
  limitMs_ = limitMs;
}

void GameBar::showState() {
  const auto &a = board_->attached();
  label_->setText(a ? a->name + dot + a->bottle + dot + QString::number(a->bits) + "-bit" : "no game attached");
  pick_->setEnabled(!board_->working());
  detach_->setEnabled(a.has_value() && !board_->working());
}

void GameBar::attach(const QString &bottle, const QString &name) {
  stopWaiting();
  board_->run("attach", {name}, {{"bottle", bottle}});
}

void GameBar::detach() {
  stopWaiting();
  board_->run("detach");
}

void GameBar::launch(const QString &bottle, const QString &path) {
  stopWaiting();
  const int gen = generation_;
  board_->run("launch", {path}, {{"bottle", bottle}}, [this, gen, bottle, path](const Reply &r) {
    if (!r.ok || gen != generation_) return;
    const QString name = r.result.toObject().value("name").toString();
    startWaiting(bottle, name.isEmpty() ? baseName(path) : name);
  });
}

void GameBar::startWaiting(const QString &bottle, const QString &name) {
  waiting_ = true;
  ++generation_;
  waitBottle_ = bottle;
  waitName_ = name;
  waited_ = 0;
  board_->say("waiting for " + name + " to start" + ellipsis);
  step_.start(stepMs_);
  look();
}

void GameBar::stopWaiting() {
  step_.stop();
  waiting_ = false;
  ++generation_;
}

// one step: the first comes at once, then one every step until the limit
void GameBar::look() {
  if (!waiting_) return;
  if (waited_ >= limitMs_) {
    const QString name = waitName_, bottle = waitBottle_;
    stopWaiting();
    board_->say(name + " did not show up in " + bottle + " after " + QString::number(limitMs_ / 1000) +
                " seconds. pick it once it is running.");
    return;
  }
  waited_ += stepMs_;
  board_->say("waiting for " + waitName_ + " to start" + ellipsis);  // an event may have replaced it
  const int gen = generation_;
  const QString bottle = waitBottle_, name = waitName_;
  board_->read("ps", {}, {{"bottle", bottle}}, [this, gen, bottle, name](const Reply &r) {
    if (gen != generation_ || !r.ok) return;
    for (const QJsonValue &p : r.result.toArray()) {
      const QString listed = p.toObject().value("name").toString();
      if (listed.compare(name, Qt::CaseInsensitive) == 0) {
        attach(bottle, listed);
        return;
      }
    }
  });
}

QString GameBar::notSent() const {
  return board_->working() ? "moth is busy: " + board_->what() : QString::fromUtf8("moth is starting…");
}

// whichever reply comes fills the menu, however many times it was opened meanwhile
void GameBar::fillMenu() {
  menu_->clear();
  separator_ = nullptr;
  QPointer<GameBar> self(this);
  if (!board_->read("bottles", {}, {}, [self](const Reply &r) { if (self) self->fillTop(r); })) {
    menu_->addAction(notSent())->setEnabled(false);
    return;
  }
  menu_->addAction(QString::fromUtf8("looking…"))->setEnabled(false);
}

void GameBar::fillTop(const Reply &r) {
  menu_->clear();
  if (!r.ok) {
    menu_->addAction(r.error)->setEnabled(false);
  } else {
    QPointer<GameBar> self(this);
    for (const QJsonValue &b : r.result.toArray()) {
      const QString bottle = b.toString();
      QMenu *sub = menu_->addMenu(bottle);
      connect(sub, &QMenu::aboutToShow, this, [self, sub, bottle] { if (self) self->fillBottle(sub, bottle); });
    }
  }
  separator_ = menu_->addSeparator();
  QPointer<GameBar> self(this);
  if (!board_->read("games", {}, {}, [self](const Reply &g) { if (self) self->fillGames(g); }))
    menu_->addAction(notSent())->setEnabled(false);
}

// what follows the separator is replaced, so a late reply never doubles the entries
void GameBar::fillGames(const Reply &g) {
  if (!separator_) return;
  const auto actions = menu_->actions();
  for (int i = actions.indexOf(separator_) + 1; i < actions.size(); ++i) menu_->removeAction(actions[i]);
  QPointer<GameBar> self(this);
  if (!g.ok) {
    menu_->addAction(g.error)->setEnabled(false);
  } else {
    for (const QJsonValue &v : g.result.toArray()) {
      const QJsonObject o = v.toObject();
      const QString name = o.value("name").toString(), bottle = o.value("bottle").toString(),
                    path = o.value("path").toString();
      menu_->addAction(name + dot + bottle, this, [self, bottle, path] { if (self) self->launch(bottle, path); });
    }
  }
  menu_->addAction(QString::fromUtf8("another game…"), this, [self] { if (self) self->askForAnotherGame(); });
}

void GameBar::fillBottle(QMenu *submenu, const QString &bottle) {
  submenu->clear();
  QPointer<GameBar> self(this);
  QPointer<QMenu> sub(submenu);
  const bool sent = board_->read("ps", {}, {{"bottle", bottle}}, [self, sub, bottle](const Reply &r) {
    if (!self || !sub) return;
    sub->clear();
    if (!r.ok) {
      sub->addAction(r.error)->setEnabled(false);
      return;
    }
    const QJsonArray procs = r.result.toArray();
    if (procs.isEmpty()) sub->addAction(r.text)->setEnabled(false);
    for (const QJsonValue &p : procs) {
      const QString name = p.toObject().value("name").toString();
      sub->addAction(name, self.data(), [self, bottle, name] { if (self) self->attach(bottle, name); });
    }
  });
  if (sent) submenu->addAction(QString::fromUtf8("looking…"))->setEnabled(false);
  else submenu->addAction(notSent())->setEnabled(false);
}

void GameBar::askForAnotherGame() {
  const QString path = QFileDialog::getOpenFileName(this, "pick a game", QString(), "programs (*.exe *.EXE)");
  if (path.isEmpty()) return;
  const bool sent = board_->read("bottles", {}, {}, [self = QPointer<GameBar>(this), path](const Reply &r) {
    if (!self) return;
    QStringList bottles;
    for (const QJsonValue &b : r.result.toArray()) bottles << b.toString();
    if (bottles.isEmpty()) {
      self->board_->say(r.ok ? "there are no bottles to launch it in." : r.error);
      return;
    }
    bool ok = false;
    const QString bottle = QInputDialog::getItem(self, "pick a bottle", "bottle", bottles, 0, false, &ok);
    if (ok) self->launch(bottle, path);
  });
  if (!sent) board_->say(notSent());
}
