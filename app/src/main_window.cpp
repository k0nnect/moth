#include "main_window.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QSplitter>
#include <QVBoxLayout>

#include "board.hpp"
#include "game_bar.hpp"
#include "kept_panel.hpp"
#include "paths.hpp"
#include "results_panel.hpp"
#include "search_bar.hpp"
#include "term_screen.hpp"
#include "terminal_view.hpp"

namespace {

constexpr int columnWidth = 520, columnLeast = 420;

// a label whose text is cut short on the right, with an ellipsis, when it does not fit; text()
// stays whole
class ElidedLabel : public QLabel {
public:
  explicit ElidedLabel(QWidget *parent) : QLabel(parent) {
    setTextFormat(Qt::PlainText);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setPen(palette().color(foregroundRole()));
    p.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
               fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width()));
  }
};

// the modifier Qt reports for Command: Control, unless the application keeps Control and Meta as they are
Qt::KeyboardModifier commandModifier() {
#ifdef Q_OS_MACOS
  return QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta) ? Qt::MetaModifier : Qt::ControlModifier;
#else
  return Qt::NoModifier;
#endif
}

// Between the command and the shell that follows: a command that stopped (Claude Code stops itself
// on Control+Z and says to run fg) is resumed until it is really gone, so it bounces back instead
// of being stranded by the exec. A status above 128 is a stop (or a death by signal); fg resumes
// the job and answers its next status, so the calls go on while it stops again and end once it
// ends or fg finds no job, which fg says to /dev/null. A function that calls itself, not a loop:
// bash, as /bin/sh, leaves a loop as soon as a job it resumed there stops again. A command that
// ended by itself (128 or less) runs none of it.
const char *const resumeStopped =
    "[ $? -gt 128 ] && { moth_resume() { fg >/dev/null 2>&1; [ $? -gt 128 ] && moth_resume; }; moth_resume; }";

}  // namespace

MainWindow::MainWindow(Board *board) : board_(board) {
  setWindowTitle("moth");
  resize(1280, 860);

  split_ = new QSplitter(this);
  columnWidget_ = new QWidget(split_);
  auto *outer = new QVBoxLayout(columnWidget_);
  column_ = new QVBoxLayout;
  outer->addLayout(column_, 1);
  column_->addWidget(new GameBar(board, columnWidget_));
  column_->addWidget(new SearchBar(board, columnWidget_));
  column_->addWidget(new ResultsPanel(board, columnWidget_), 1);
  column_->addWidget(new KeptPanel(board, columnWidget_));
  line_ = new QLabel(columnWidget_);
  line_->setObjectName("line");
  line_->setTextFormat(Qt::PlainText);  // the core's words as they are: `<bottle>` is not markup
  line_->setWordWrap(true);
  outer->addWidget(line_);
  split_->addWidget(columnWidget_);
  split_->setStretchFactor(0, 0);
  setCentralWidget(split_);
  buildTerminal();

  connect(board, &Board::changed, this, [this] {
    showLine();
    syncQuiet();
  });
  poll_.setInterval(1000);
  connect(&poll_, &QTimer::timeout, board, &Board::tick);
  showLine();
}

MainWindow::~MainWindow() { dropTerminal(); }

QVBoxLayout *MainWindow::column() { return column_; }
TerminalView *MainWindow::terminal() { return terminal_; }

void MainWindow::setTerminal(bool on) {
  if (!on && pane_) {
    dropTerminal();
    resize(560, 860);
  }
}

// the strip and the view, on the right of the splitter
void MainWindow::buildTerminal() {
  pane_ = new QWidget(split_);
  auto *box = new QVBoxLayout(pane_);
  box->setContentsMargins(0, 0, 0, 0);
  box->setSpacing(0);
  auto *strip = new QWidget(pane_);
  auto *row = new QHBoxLayout(strip);
  row->setContentsMargins(8, 4, 8, 4);
  title_ = new ElidedLabel(strip);
  title_->setObjectName("terminalTitle");
  quiet_ = new QCheckBox("quiet", strip);
  quiet_->setObjectName("quiet");
  row->addWidget(title_, 1);
  row->addWidget(quiet_);
  box->addWidget(strip);
  terminal_ = new TerminalView(pane_);
  terminal_->setObjectName("terminal");
  terminal_->installEventFilter(this);
  box->addWidget(terminal_, 1);
  split_->addWidget(pane_);
  split_->setChildrenCollapsible(false);
  columnWidget_->setMinimumWidth(columnLeast);
  split_->setStretchFactor(1, 1);
  split_->setSizes({columnWidth, width() - columnWidth});

  connect(terminal_->screen(), &TermScreen::titleChanged, this, [this] { showTitle(); });
  connect(terminal_, &TerminalView::exited, this, [this] {
    ended_ = true;
    title_->setText("the terminal ended. press return in it to start it again.");
  });
  connect(quiet_, &QCheckBox::clicked, this, &MainWindow::quietClicked);
  showTitle();
  syncQuiet();
}

// the terminal goes before anything else of the window: its child is hung up on and reaped here
void MainWindow::dropTerminal() {
  split_->setChildrenCollapsible(true);
  columnWidget_->setMinimumWidth(0);
  delete pane_;  // and the view in it
  pane_ = nullptr;
  title_ = nullptr;
  quiet_ = nullptr;
  terminal_ = nullptr;
}

void MainWindow::startTerminal() {
  if (!terminal_ || terminal_->running()) return;
  const QString shell = userShell(), command = terminalCommand(mothBin());
  QStringList args{"-l", "-i"};
  if (!command.isEmpty()) args << "-c" << command + "\n" + resumeStopped + "\nexec \"$SHELL\" -l -i";
  QString home = qEnvironmentVariable("HOME");
  if (home.isEmpty()) home = "/";
  started_ = true;
  ended_ = !terminal_->run(shell, args, terminalEnv(board_->home(), mothBin()), home);
  if (ended_) title_->setText("the terminal could not start " + shell + ". press return in it to try again.");
  else showTitle();
}

void MainWindow::showTitle() {
  QString title = terminal_->screen()->title();
  // a line or paragraph separator, or any control character, would make the strip two lines tall
  for (QChar &c : title)
    if (c == QChar::LineSeparator || c == QChar::ParagraphSeparator || c.category() == QChar::Other_Control) c = ' ';
  if (!ended_) title_->setText(title.isEmpty() ? "terminal" : title);
}

// the checkbox shows what the board says, and is out of reach while the board is busy or stale
void MainWindow::syncQuiet() {
  if (!quiet_) return;
  const QSignalBlocker block(quiet_);
  quiet_->setChecked(board_->quiet());
  quiet_->setEnabled(!board_->working() && !board_->stale());
}

// a click asks the board; the box stays as it was until the board says otherwise
void MainWindow::quietClicked(bool on) {
  syncQuiet();
  board_->run("quiet", {on ? "on" : "off"});
}

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
  if (terminal_ && !started_) {
    startTerminal();
    terminal_->setFocus();
  }
}

void MainWindow::closeEvent(QCloseEvent *e) {
  dropTerminal();
  QMainWindow::closeEvent(e);
}

// In a terminal whose shell has ended Return starts it again, Command+C and Command+A go to the
// view as usual (the last screen can be copied), and any other key does nothing
bool MainWindow::eventFilter(QObject *watched, QEvent *e) {
  if (watched == terminal_ && ended_ && e->type() == QEvent::KeyPress) {
    const auto *k = static_cast<QKeyEvent *>(e);
    if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) startTerminal();
    else if ((k->key() == Qt::Key_C || k->key() == Qt::Key_A) && k->modifiers() == commandModifier()) return false;
    return true;
  }
  return QMainWindow::eventFilter(watched, e);
}

void MainWindow::hideEvent(QHideEvent *e) {
  QMainWindow::hideEvent(e);
  poll_.stop();
}

void MainWindow::showLine() {
  line_->setText(!board_->stale() && board_->working() ? "working: " + board_->what() + QString::fromUtf8("…") : board_->line());
}
