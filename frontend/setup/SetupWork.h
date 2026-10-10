#pragma once

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <string>
#include <vector>

class QWidget;

namespace mira_gui {

class DownloadTracker;

// Set up Mira's slow work, started as soon as its page is left with Next so it's done or under way
// by the time it's needed: system packages, Proton, the stores' helper tools, Microsoft 365's
// Windows setup and its apps, and the Steam and Lutris imports. Owned by the main window, so it
// keeps going after the setup window closes.
class SetupWork : public QObject {
  Q_OBJECT

 public:
  SetupWork(DownloadTracker* downloads, QObject* parent);

  // `install` is mirad's command for `packages` (GET /v1/system/packages). Asks for the password
  // through `window`.
  void InstallPackages(QWidget* window, const std::vector<std::string>& install,
                       const std::vector<std::string>& packages, bool restart);
  void DownloadProton();
  // Each store's helper tool that isn't there yet.
  void SetUpTools(const QStringList& stores);
  // Microsoft 365's prefix, once the packages are in.
  void PrepareOffice(QWidget* window);
  // Added once Office is ready, or now if it is.
  void AddOfficeApps(const std::vector<std::string>& refs);
  void ImportSteam();
  void ImportLutris();
  // Turns each source on or off (`<id>.enabled`).
  void SetSourcesOn(const QStringList& on, const QStringList& off);

  // What's under way, one short line each: "Installing packages", "Microsoft 365 · 34%".
  QStringList Running() const;
  bool OfficeStarted() const { return office_started_; }
  bool OfficeFailed() const { return office_failed_; }

 signals:
  void Changed();
  // A store, launcher or library change worth refreshing the sidebar's sources for.
  void SourcesChanged();

 private:
  void HandleEvent(const std::string& type, const std::string& data);
  void StartOffice();

  DownloadTracker* downloads_ = nullptr;
  QPointer<QWidget> office_window_;  // asks for Office's packages, if any are still missing
  bool packages_running_ = false;
  bool proton_running_ = false;
  bool office_waiting_ = false;  // for the packages
  bool office_started_ = false;
  bool office_accepted_ = false;  // mirad started the install
  std::vector<std::string> office_apps_;  // asked for before that
  bool office_running_ = false;
  bool office_failed_ = false;
  double office_progress_ = -1;
  bool steam_running_ = false;
  bool lutris_running_ = false;
  QSet<QString> tools_running_;
};

}  // namespace mira_gui
