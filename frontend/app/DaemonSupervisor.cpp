#include "DaemonSupervisor.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <QTimer>
#include <csignal>
#include <string_view>
#include <utility>

#ifdef __APPLE__
#include <libproc.h>
#endif

#include "../client/api/Config.h"

namespace mira_gui {

namespace {
constexpr int kPollIntervalMs = 150;
constexpr int kPollAttempts = 30;                            // ~4.5s total
constexpr const char* kHandOffVariable = "MIRA_DAEMON_PID";  // set by display::Restart
}  // namespace

QString ResolveMiradPath(const QString& own_binary_dir) {
  const QString candidate = own_binary_dir + "/mirad";
  return QFileInfo::exists(candidate) ? candidate : QString("mirad");
}

DaemonSupervisor::DaemonSupervisor(QObject* parent) : QObject(parent) {
  connect(qApp, &QCoreApplication::aboutToQuit, this, &DaemonSupervisor::StopIfSelfStarted);
}

DaemonSupervisor::~DaemonSupervisor() {
  StopIfSelfStarted();
}

void DaemonSupervisor::EnsureRunning() {
  api::CheckHealthAsync(this, [this](HealthStatus status) {
    if (status.reachable) {
      // Handed over by the frontend this one replaced.
      if (const qint64 pid = qEnvironmentVariable(kHandOffVariable).toLongLong(); pid > 0) {
        pid_ = pid;
        qunsetenv(kHandOffVariable);
      }
      if (status.api != kExpectedApiVersion) {
        emit Outdated(status.api);
        return;
      }
      emit Ready();
      return;
    }

    const QString mirad_path = ResolveMiradPath(QCoreApplication::applicationDirPath());
    // Detached, so a restart of this frontend (display::Restart) can hand it to the next one.
    // The socket this frontend talks to: mirad ignores $MIRA_SOCKET, which can move it.
    QProcess process;
    process.setProgram(mirad_path);
    process.setArguments({"--socket", QString::fromStdString(api::ResolveSocketPath())});
    qint64 pid = 0;
    if (!process.startDetached(&pid)) {
      emit Failed(QString("could not start %1 (%2)").arg(mirad_path, process.errorString()));
      return;
    }
    pid_ = pid;
    PollHealth(kPollAttempts);
  });
}

void DaemonSupervisor::PollHealth(int attempts_left) {
  api::CheckHealthAsync(this, [this, attempts_left](HealthStatus status) {
    if (status.reachable) {
      emit Ready();
      return;
    }
    if (attempts_left <= 0) {
      emit Failed("mirad did not become reachable after starting it");
      return;
    }
    QTimer::singleShot(kPollIntervalMs, this,
                       [this, attempts_left] { PollHealth(attempts_left - 1); });
  });
}

void DaemonSupervisor::HandOff() {
  pid_ = 0;
}

void DaemonSupervisor::StopIfSelfStarted() {
  const qint64 pid = std::exchange(pid_, 0);
  // Nothing watches a detached mirad, so its pid may have gone to another process since.
  const auto running = [pid] {
#ifdef __APPLE__
    char name[2 * MAXCOMLEN + 1] = {};
    return proc_name(int(pid), name, sizeof(name)) > 0 && std::string_view(name) == "mirad";
#else
    QFile comm(QString("/proc/%1/comm").arg(pid));
    return comm.open(QIODevice::ReadOnly) && comm.readAll().trimmed() == "mirad";
#endif
  };
  if (pid <= 0 || !running() || ::kill(pid_t(pid), SIGTERM) != 0) return;
  for (int i = 0; i < 20 && running(); ++i) QThread::msleep(100);
  if (running()) ::kill(pid_t(pid), SIGKILL);
}

}  // namespace mira_gui
