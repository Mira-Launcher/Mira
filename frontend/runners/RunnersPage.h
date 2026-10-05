#pragma once

#include <QWidget>

#include <map>
#include <string>
#include <vector>

#include "../client/Types.h"

class QBoxLayout;
class QButtonGroup;
class QComboBox;
class QLabel;

namespace mira_gui {

class DownloadTracker;
class SettingsCard;

// The library window's Runners page: the Proton or Wine builds installed,
// which one Windows games default to, updates for them, the builds available
// to download from each source, and the tools runners need.
// Rebuilt on every open, so it starts from mirad's current state.
class RunnersPage : public QWidget {
  Q_OBJECT

public:
  explicit RunnersPage(DownloadTracker* downloads, QWidget* parent = nullptr);

  // The whole library, to say how many games use each build.
  void SetGames(const std::vector<GameSummary>& games);

protected:
  void resizeEvent(QResizeEvent* event) override;

private:
  std::string CurrentKind() const;
  void Refresh();
  void RefreshInstalled();
  void RefreshCatalog();
  void RefreshSources();
  void RefreshUpdates();
  void RefreshTools();
  void RebuildInstalled();
  void RebuildCatalog();
  void RebuildTools();
  void Update(const RunnerInfo& runner, const RunnerUpdate& update);
  void SetupTool(const RunnerTool& tool);
  QString SourceLabel(const std::string& id) const;
  void SetDefault(const std::string& reference);
  void Remove(const RunnerInfo& runner);
  void Download(const std::string& tag);
  void DownloadChanged(const QString& key);
  void SetStatus(const QString& text, bool error = false);

  DownloadTracker* downloads_ = nullptr;
  QButtonGroup* kinds_ = nullptr;
  QBoxLayout* columns_ = nullptr;  // side by side, or stacked on a narrow window
  SettingsCard* installed_ = nullptr;
  QLabel* default_note_ = nullptr;
  SettingsCard* catalog_ = nullptr;
  QComboBox* source_ = nullptr;
  SettingsCard* tools_card_ = nullptr;  // shown only while a tool is missing
  QLabel* status_ = nullptr;

  std::vector<GameSummary> games_;
  std::vector<RunnerInfo> runners_;  // every kind, as last listed
  std::vector<RunnerRelease> releases_;  // the chosen source's
  std::map<std::string, std::vector<RunnerSourceInfo>> sources_;  // by kind
  std::vector<RunnerUpdate> updates_;
  std::vector<RunnerTool> tools_;
  std::map<std::string, std::string> replacing_;  // new release name -> reference it updates
  bool catalog_loaded_ = false;
  std::string default_windows_;  // default_runner.windows
};

}  // namespace mira_gui
