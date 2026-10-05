#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

namespace mira_gui {

// Same resolution `mira daemon` uses (src/cli/main.cpp): mirad next to
// own_binary_dir if present there (true inside an AppImage, where mirad and
// mira-gui sit in the same usr/bin/), else a bare name for QProcess/PATH
// lookup. A free function so it's testable without spawning anything.
QString ResolveMiradPath(const QString& own_binary_dir);

// Implements docs/architecture.md's "frontend-managed" daemon path: if
// mirad is already reachable, do nothing. If not, spawn it next to this
// binary and supervise it, but only tear down a daemon *we* started; one
// found already running (systemd or otherwise) is left alone regardless of
// how this frontend exits.
class DaemonSupervisor : public QObject {
  Q_OBJECT
 public:
  explicit DaemonSupervisor(QObject* parent = nullptr);
  ~DaemonSupervisor() override;

  void EnsureRunning();

 signals:
  void Ready();
  void Failed(QString error);
  void Outdated(int api);  // a running mirad speaks a different API version

 private:
  void PollHealth(int attempts_left);
  void StopIfSelfStarted();

  QProcess* process_ = nullptr;
  bool self_started_ = false;
};

}  // namespace mira_gui
