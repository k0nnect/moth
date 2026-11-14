#include "pty.hpp"

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QSocketNotifier>
#include <QTimer>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>  // forkpty, on macOS

namespace {

winsize sizeOf(int rows, int cols) {
  winsize ws{};
  ws.ws_row = static_cast<unsigned short>(rows);
  ws.ws_col = static_cast<unsigned short>(cols);
  return ws;
}

// the child's side of the fork: only calls that are safe between fork and exec
[[noreturn]] void childMain(const char *cwd, char *const *argv, char *const *envp) {
  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, nullptr);
  for (int sig = 1; sig < NSIG; ++sig) signal(sig, SIG_DFL);
  if (chdir(cwd) != 0) _exit(127);
  int limit = getdtablesize();
  if (limit > 65536) limit = 65536;
  for (int fd = 3; fd < limit; ++fd) close(fd);
  execve(argv[0], argv, envp);
  _exit(127);
}

}  // namespace

Pty::Pty(QObject *parent) : QObject(parent) {}

Pty::~Pty() {
  // The terminal's foreground group, read while the line is still ours. A job of an interactive
  // shell (the agent) runs in a group of its own, which the shell's own end does not take with
  // it; it is treated as the shell's group is. Not a real group (the call failed, or init's), the
  // shell's own or ours: left alone.
  pid_t job = -1;
  if (master_ >= 0 && pid_ > 0) {
    job = tcgetpgrp(master_);
    if (job <= 1 || job == pid_ || job == getpgrp()) job = -1;
  }
  // Let go of the line first. On macOS a session leader's exit waits for the tty's output to be
  // read, so with unread output the child cannot be reaped until the master is closed; closing it
  // also hangs up the line.
  delete readNotifier_;
  delete writeNotifier_;
  readNotifier_ = writeNotifier_ = nullptr;
  if (master_ >= 0) ::close(master_);
  master_ = -1;
  if (pid_ <= 0) return;
  int status = 0;
  // ECHILD: already reaped (the child's exit was seen), so there is nothing to signal or wait for
  bool shellGone = false;
  auto gone = [&] {
    if (!shellGone) {
      const pid_t r = waitpid(pid_, &status, WNOHANG);
      shellGone = r == pid_ || (r < 0 && errno == ECHILD);
    }
    // the job's group is gone when no process is left in it
    return shellGone && (job < 0 || (kill(-job, 0) != 0 && errno == ESRCH));
  };
  auto signalBoth = [&](int sig) {
    if (!shellGone) kill(-pid_, sig);
    if (job > 0) kill(-job, sig);
  };
  bool over = gone();
  if (!over) {
    signalBoth(SIGHUP);
    signalBoth(SIGCONT);  // a stopped child would not see the hangup
    for (int waited = 0; waited < 500 && !over; waited += 5) {
      over = gone();
      if (!over) usleep(5000);
    }
  }
  if (!over) {
    signalBoth(SIGKILL);
    for (int waited = 0; waited < 1000 && !over; waited += 5) {  // never wait for ever
      over = gone();
      if (!over) usleep(5000);
    }
  }
}

QString Pty::find(const QString &program, const QStringList &env) const {
  if (program.contains('/')) return program;
  QString path = qEnvironmentVariable("PATH");
  for (const QString &e : env)
    if (e.startsWith("PATH=")) path = e.mid(5);
  for (const QString &dir : path.split(':', Qt::SkipEmptyParts)) {
    QFileInfo fi(QDir(dir).filePath(program));
    if (fi.isFile() && fi.isExecutable()) return fi.filePath();
  }
  return {};
}

bool Pty::start(const QString &program, const QStringList &args, const QStringList &env,
                const QString &cwd, int rows, int cols) {
  if (pid_ > 0) return false;
  const QString exe = find(program, env);
  QFileInfo fi(exe);
  fi = QFileInfo(fi.absoluteFilePath());  // the app's folder, as the child will not be in it
  if (exe.isEmpty() || !fi.isFile() || !fi.isExecutable()) return false;
  if (!cwd.isEmpty() && !QFileInfo(cwd).isDir()) return false;

  // everything the child needs, built before the fork
  const QByteArray exeB = QFile::encodeName(fi.absoluteFilePath());
  const QByteArray cwdB = QFile::encodeName(cwd.isEmpty() ? QStringLiteral(".") : cwd);
  QList<QByteArray> argStore{exeB};
  for (const QString &a : args) argStore << a.toUtf8();
  QList<QByteArray> envStore;
  for (const QString &e : env) envStore << e.toUtf8();
  QList<char *> argv, envp;
  for (QByteArray &a : argStore) argv << a.data();
  argv << nullptr;
  for (QByteArray &e : envStore) envp << e.data();
  envp << nullptr;

  winsize ws = sizeOf(rows, cols);
  // the kernel's own defaults, and UTF-8 on, so that Backspace in a line editor erases a character
  // (read from a fresh pty, then IUTF8 added)
  // when either call fails the kernel's own defaults are used as they are
  termios tio{};
  bool haveTermios = false;
  int m = -1, sl = -1;
  if (openpty(&m, &sl, nullptr, nullptr, nullptr) == 0) {
    haveTermios = tcgetattr(sl, &tio) == 0;
    ::close(m);
    ::close(sl);
  }
  tio.c_iflag |= IUTF8;
  int fd = -1;
  const pid_t pid = forkpty(&fd, nullptr, haveTermios ? &tio : nullptr, &ws);
  if (pid < 0) return false;
  if (pid == 0) childMain(cwdB.constData(), argv.data(), envp.data());

  master_ = fd;
  pid_ = pid;
  ending_ = false;
  fcntl(master_, F_SETFD, FD_CLOEXEC);
  fcntl(master_, F_SETFL, fcntl(master_, F_GETFL) | O_NONBLOCK);
  readNotifier_ = new QSocketNotifier(master_, QSocketNotifier::Read, this);
  connect(readNotifier_, &QSocketNotifier::activated, this, &Pty::readable);
  writeNotifier_ = new QSocketNotifier(master_, QSocketNotifier::Write, this);
  writeNotifier_->setEnabled(false);
  connect(writeNotifier_, &QSocketNotifier::activated, this, &Pty::writable);
  poll_ = new QTimer(this);
  poll_->setInterval(100);
  connect(poll_, &QTimer::timeout, this, &Pty::check);
  poll_->start();
  return true;
}

bool Pty::running() const { return pid_ > 0; }
qint64 Pty::pid() const { return pid_ > 0 ? pid_ : 0; }

void Pty::write(const QByteArray &bytes) {
  if (pid_ <= 0 || ending_ || bytes.isEmpty()) return;
  pending_ += bytes;
  flush();
}

void Pty::flush() {
  while (!pending_.isEmpty()) {
    const ssize_t n = ::write(master_, pending_.constData(), static_cast<size_t>(pending_.size()));
    if (n > 0) {
      pending_.remove(0, static_cast<qsizetype>(n));
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    } else {
      pending_.clear();  // the child's side is gone: check() will find the exit
      break;
    }
  }
  writeNotifier_->setEnabled(!pending_.isEmpty());
}

void Pty::writable() { flush(); }

void Pty::resize(int rows, int cols) {
  if (pid_ <= 0) return;
  const winsize ws = sizeOf(rows, cols);
  ioctl(master_, TIOCSWINSZ, &ws);  // the kernel sends the child SIGWINCH
}

void Pty::readable() {
  char buf[65536];
  const ssize_t n = ::read(master_, buf, sizeof buf);
  if (n > 0) {
    emit output(QByteArray(buf, static_cast<qsizetype>(n)));
  } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
    readNotifier_->setEnabled(false);  // the other end is closed: the child is going or gone
    check();
  }
}

void Pty::check() {
  if (pid_ <= 0) return;
  int status = 0;
  const pid_t r = waitpid(pid_, &status, WNOHANG);
  if (r == pid_) {
    finish(status);
  } else if (r < 0 && errno != EINTR) {
    finish(0);  // not ours to wait for any more; do not leave the pty hanging
  }
}

// false when a slot deleted this Pty while it ran
bool Pty::drain() {
  QPointer<Pty> self(this);
  char buf[65536];
  for (;;) {
    const ssize_t n = ::read(master_, buf, sizeof buf);
    if (n > 0) {
      emit output(QByteArray(buf, static_cast<qsizetype>(n)));
      if (!self) return false;
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
  return true;
}

void Pty::finish(int raw) {
  ending_ = true;  // the child is gone: whatever a slot writes now has nowhere to go
  poll_->stop();
  readNotifier_->setEnabled(false);
  writeNotifier_->setEnabled(false);
  pending_.clear();
  // what the child wrote last may still be in the pty when its exit is seen. (On macOS the session
  // leader cannot be reaped before its output is read, so no test can show this drain here; it
  // matters on systems where it can.)
  if (!drain()) return;
  pid_ = 0;
  ::close(master_);
  master_ = -1;
  // not deleted here: this may run inside one of their own signals
  readNotifier_->deleteLater();
  writeNotifier_->deleteLater();
  poll_->deleteLater();
  readNotifier_ = writeNotifier_ = nullptr;
  poll_ = nullptr;
  int code = 0;
  if (WIFEXITED(raw)) code = WEXITSTATUS(raw);
  else if (WIFSIGNALED(raw)) code = 128 + WTERMSIG(raw);
  emit exited(code);
}
