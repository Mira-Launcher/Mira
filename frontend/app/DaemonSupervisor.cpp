#include "DaemonSupervisor.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QTimer>

#include "../client/MiradClient.h"

namespace mira_gui {

namespace {
constexpr int kPollIntervalMs = 150;
constexpr int kPollAttempts = 30;  // ~4.5s total
}  // namespace

QString ResolveMiradPath(const QString& own_binary_dir) {
  const QString candidate = own_binary_dir + "/mirad";
  return QFileInfo::exists(candidate) ? candidate : QString("mirad");
}

DaemonSupervisor::DaemonSupervisor(QObject* parent) : QObject(parent) {
  connect(qApp, &QCoreApplication::aboutToQuit, this, &DaemonSupervisor::StopIfSelfStarted);
}

DaemonSupervisor::~DaemonSupervisor() { StopIfSelfStarted(); }

void DaemonSupervisor::EnsureRunning() {
  MiradClient::CheckHealthAsync(this, [this](HealthStatus status) {
    if (status.reachable) {
      if (status.api != kExpectedApiVersion) {
        emit Outdated(status.api);
        return;
      }
      emit Ready();
      return;
    }

    const QString mirad_path = ResolveMiradPath(QCoreApplication::applicationDirPath());
    if (process_ != nullptr) process_->deleteLater();  // a previous mirad that has since died
    process_ = new QProcess(this);
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
      qWarning("mirad: %s", process_->readAllStandardError().constData());
    });
    // The socket this frontend talks to: mirad ignores $MIRA_SOCKET, which can move it.
    process_->start(mirad_path, {"--socket", QString::fromStdString(MiradClient::ResolveSocketPath())});
    if (!process_->waitForStarted(2000)) {
      emit Failed(QString("could not start %1 (%2)").arg(mirad_path, process_->errorString()));
      return;
    }
    self_started_ = true;
    PollHealth(kPollAttempts);
  });
}

void DaemonSupervisor::PollHealth(int attempts_left) {
  MiradClient::CheckHealthAsync(this, [this, attempts_left](HealthStatus status) {
    if (status.reachable) {
      emit Ready();
      return;
    }
    if (attempts_left <= 0) {
      emit Failed("mirad did not become reachable after starting it");
      return;
    }
    QTimer::singleShot(kPollIntervalMs, this, [this, attempts_left] { PollHealth(attempts_left - 1); });
  });
}

void DaemonSupervisor::StopIfSelfStarted() {
  if (!self_started_ || !process_) return;
  self_started_ = false;
  process_->terminate();
  if (!process_->waitForFinished(2000)) process_->kill();
}

}  // namespace mira_gui
