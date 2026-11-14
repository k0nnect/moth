#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <sys/types.h>

class QSocketNotifier;
class QTimer;

// a child process on a pseudo-terminal. nothing here blocks except the destructor, briefly.
// The child gets its own session with the pty as its controlling terminal; its output arrives as
// `output`, in order, and `exited` comes once, after the last of it.
class Pty : public QObject {
  Q_OBJECT
public:
  explicit Pty(QObject *parent = nullptr);
  // SIGHUP to the child's process group and to the terminal's foreground group (a job of the
  // child's, such as the agent); SIGKILL to whichever is still there after 500 ms. 1.5 s at most
  ~Pty() override;
  // env is the whole environment of the child (each "NAME=value"). false, with nothing started,
  // when the program cannot be run (not found, not executable, or cwd is not a folder). A program
  // without a slash is looked for on the PATH of env, else on ours.
  bool start(const QString &program, const QStringList &args, const QStringList &env,
             const QString &cwd, int rows, int cols);
  // bytes for the child's input; does nothing before start() or after the child has gone
  void write(const QByteArray &bytes);
  void resize(int rows, int cols);
  bool running() const;
  qint64 pid() const;  // 0 when no child is running
signals:
  void output(const QByteArray &bytes);
  void exited(int status);  // the exit code, or 128 + the signal

private:
  void readable();
  void writable();
  void flush();
  void check();                 // has the child ended? when it has, drain, reap, emit
  bool drain();
  void finish(int rawStatus);
  QString find(const QString &program, const QStringList &env) const;

  int master_ = -1;
  pid_t pid_ = 0;
  bool ending_ = false;         // the child has been found gone: nothing more is written
  QByteArray pending_;          // input the pty would not take yet
  QSocketNotifier *readNotifier_ = nullptr, *writeNotifier_ = nullptr;
  QTimer *poll_ = nullptr;      // the child can end with the pty still held open by another process
};
