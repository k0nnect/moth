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
#include <utility>

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
  // the launched program is attached (whoever attached it): the wait is over
  connect(board, &Board::changed, this, [this] {
    if (arrived()) stopWaiting();
  });
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

QString GameBar::waitingLine() const { return "waiting for " + waitName_ + " to start" + ellipsis; }

bool GameBar::arrived() const {
  const auto &a = board_->attached();
  return waiting_ && a && a->name.compare(waitName_, Qt::CaseInsensitive) == 0;
}

void GameBar::startWaiting(const QString &bottle, const QString &name) {
  waiting_ = true;
  ++generation_;
  waitBottle_ = bottle;
  waitName_ = name;
  waited_ = 0;
  board_->say(waitingLine());
  step_.start(stepMs_);
  look();
}

void GameBar::stopWaiting() {
  step_.stop();
  waiting_ = false;
  ++generation_;
}

// One step: the first comes at once, then one every step until the limit. Once the program is
// listed in its bottle it is attached; the wait ends when the board shows it attached, so an
// attach that is refused, or not sent because moth is busy, is tried again at the next step.
void GameBar::look() {
  if (!waiting_) return;
  if (arrived()) {
    stopWaiting();
    return;
  }
  if (waited_ >= limitMs_) {
    const QString name = waitName_, bottle = waitBottle_;
    stopWaiting();
    board_->say(name + " did not show up in " + bottle + " after " + QString::number(limitMs_ / 1000) + " seconds. see " +
                board_->home() + "/launch.log, or pick it once it is running.");
    return;
  }
  waited_ += stepMs_;
  // said again when an event replaced it with nothing, but never over what the user's own action
  // said, such as why an attach was refused
  if (board_->line().isEmpty() || board_->line() == waitingLine()) board_->say(waitingLine());
  const int gen = generation_;
  const QString bottle = waitBottle_, name = waitName_;
  readPs(bottle, [this, gen, bottle, name](const Reply &r) {
    if (gen != generation_ || !r.ok || !board_->busyText().isEmpty()) return;
    for (const QJsonValue &p : r.result.toArray()) {
      const QString listed = p.toObject().value("name").toString();
      if (listed.compare(name, Qt::CaseInsensitive) == 0) {
        board_->run("attach", {listed}, {{"bottle", bottle}});
        return;
      }
    }
  });
}

// every `ps` the bar sends goes through here: one the same as a ps still out is not sent again
// (Board::read would answer only the first caller), and its reply goes to every caller
bool GameBar::readPs(const QString &bottle, std::function<void(const Reply &)> then) {
  if (psOut_.contains(bottle)) {
    psOut_[bottle].append(std::move(then));
    return true;
  }
  QPointer<GameBar> self(this);
  const bool sent = board_->read("ps", {}, {{"bottle", bottle}}, [self, bottle](const Reply &r) {
    if (!self) return;
    for (const auto &f : self->psOut_.take(bottle))
      if (f) f(r);
  });
  if (sent) psOut_.insert(bottle, {std::move(then)});
  return sent;
}

// whichever reply comes fills the menu, however many times it was opened meanwhile
void GameBar::fillMenu() {
  menu_->clear();
  separator_ = nullptr;
  QPointer<GameBar> self(this);
  if (!board_->read("bottles", {}, {}, [self](const Reply &r) { if (self) self->fillTop(r); })) {
    menu_->addAction(board_->busyText())->setEnabled(false);
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
      // closed before its turn to look: it no longer waits for one
      connect(sub, &QMenu::aboutToHide, this, [self, sub] { if (self && self->next_ == sub) self->next_ = nullptr; });
    }
  }
  separator_ = menu_->addSeparator();
  QPointer<GameBar> self(this);
  if (!board_->read("games", {}, {}, [self](const Reply &g) { if (self) self->fillGames(g); }))
    menu_->addAction(board_->busyText())->setEnabled(false);
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

// Looking in a bottle can start wine there, and the core does one thing at a time, so the menu
// looks in one bottle at a time and the line says which. A submenu opened meanwhile says whose
// turn it is and looks once that is done, if it is still the one open.
void GameBar::fillBottle(QMenu *submenu, const QString &bottle) {
  submenu->clear();
  if (!looking_.isEmpty()) {
    next_ = submenu;
    nextBottle_ = bottle;
    submenu->addAction(looking_ == bottle ? QString::fromUtf8("looking…") : "looking in " + looking_ + " first" + ellipsis)->setEnabled(false);
    return;
  }
  QPointer<GameBar> self(this);
  QPointer<QMenu> sub(submenu);
  if (!readPs(bottle, [self, sub, bottle](const Reply &r) { if (self) self->looked(sub, bottle, r); })) {
    submenu->addAction(board_->busyText())->setEnabled(false);
    return;
  }
  looking_ = bottle;
  lineBefore_ = board_->line();
  board_->say("looking in " + bottle + ellipsis);
  submenu->addAction(QString::fromUtf8("looking…"))->setEnabled(false);
}

// the menu's `ps` came back: its submenu is filled, and the line says what the core said when
// nothing is running there (or its error), else goes back to what it said before
void GameBar::looked(QMenu *submenu, const QString &bottle, const Reply &r) {
  looking_.clear();
  if (submenu) showProcesses(submenu, bottle, r);
  if (!r.ok) board_->say(r.error);
  else if (r.result.toArray().isEmpty()) board_->say(r.text);
  else if (board_->line() == "looking in " + bottle + ellipsis) board_->say(lineBefore_);
  QPointer<QMenu> next = std::exchange(next_, nullptr);
  if (!next) return;
  if (nextBottle_ == bottle) showProcesses(next, bottle, r);
  else fillBottle(next, nextBottle_);
}

void GameBar::showProcesses(QMenu *submenu, const QString &bottle, const Reply &r) {
  submenu->clear();
  if (!r.ok) {
    submenu->addAction(r.error)->setEnabled(false);
    return;
  }
  const QJsonArray procs = r.result.toArray();
  if (procs.isEmpty()) submenu->addAction(r.text)->setEnabled(false);
  QPointer<GameBar> self(this);
  for (const QJsonValue &p : procs) {
    const QString name = p.toObject().value("name").toString();
    submenu->addAction(name, this, [self, bottle, name] { if (self) self->attach(bottle, name); });
  }
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
  if (!sent) board_->say(board_->busyText());
}
