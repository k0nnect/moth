#include "pty.hpp"

#include <QDir>
#include <QFileInfo>
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
  if (pid_ <= 0) {
    if (master_ >= 0) ::close(master_);
    return;
  }
  int status = 0;
  if (waitpid(pid_, &status, WNOHANG) != pid_) {
    kill(-pid_, SIGHUP);
    kill(-pid_, SIGCONT);  // a stopped child would not see the hangup
    bool gone = false;
    for (int waited = 0; waited < 500 && !gone; waited += 5) {
      gone = waitpid(pid_, &status, WNOHANG) == pid_;
      if (!gone) usleep(5000);
    }
    if (!gone) {
      kill(-pid_, SIGKILL);
      while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    }
  }
  if (master_ >= 0) ::close(master_);
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
  if (exe.isEmpty() || !fi.isFile() || !fi.isExecutable()) return false;
  if (!cwd.isEmpty() && !QFileInfo(cwd).isDir()) return false;

  // everything the child needs, built before the fork
  const QByteArray exeB = QFile::encodeName(exe);
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
  int fd = -1;
  const pid_t pid = forkpty(&fd, nullptr, nullptr, &ws);
  if (pid < 0) return false;
  if (pid == 0) childMain(cwdB.constData(), argv.data(), envp.data());

  master_ = fd;
  pid_ = pid;
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
  if (pid_ <= 0 || bytes.isEmpty()) return;
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

void Pty::drain() {
  char buf[65536];
  for (;;) {
    const ssize_t n = ::read(master_, buf, sizeof buf);
    if (n > 0) emit output(QByteArray(buf, static_cast<qsizetype>(n)));
    else if (n < 0 && errno == EINTR) continue;
    else break;
  }
}

void Pty::finish(int raw) {
  poll_->stop();
  readNotifier_->setEnabled(false);
  writeNotifier_->setEnabled(false);
  drain();
  pid_ = 0;
  pending_.clear();
  ::close(master_);
  master_ = -1;
  int code = 0;
  if (WIFEXITED(raw)) code = WEXITSTATUS(raw);
  else if (WIFSIGNALED(raw)) code = 128 + WTERMSIG(raw);
  emit exited(code);
}
