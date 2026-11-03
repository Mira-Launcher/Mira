#pragma once

#include <QString>
#include <QWidget>
#include <set>
#include <string>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace mira_gui {

class DownloadTracker;
class MiraUpdater;
class ProgressRail;
class RunnerUpdater;
class UpdateChecker;
struct UpdateRow;

// The top bar's updates popover, laid out like Activity: Mira, then the runner builds, emulators,
// mod managers and tools Mira downloaded. A Qt::Popup, so it closes itself on an outside click or
// Escape.
class UpdatesPanel : public QWidget {
  Q_OBJECT

 public:
  UpdatesPanel(UpdateChecker* checker, MiraUpdater* mira, RunnerUpdater* runners,
               DownloadTracker* downloads, QWidget* parent);

  // Shows it with its top-right corner under `anchor`'s bottom-right.
  void ShowBelow(QWidget* anchor);

  // Starts Mira's update, showing its progress below `anchor`; an install that can't update itself
  // only shows how to.
  void StartMira(QWidget* anchor);

  // What the top bar counts: every row, Mira's included.
  int Pending() const;

 private:
  void StartMiraUpdate(const QString& latest);
  void Rebuild();
  QWidget* BuildMiraRow();
  QWidget* BuildRow(const UpdateRow& update, bool first);
  bool Updating(const UpdateRow& update) const;
  void Update(const UpdateRow& update);
  void UpdateAll();

  QWidget* anchor_ = nullptr;
  std::set<std::string> running_;  // the targets of install and setup jobs
  UpdateChecker* checker_;
  MiraUpdater* mira_;
  RunnerUpdater* runners_;
  DownloadTracker* downloads_;
  QVBoxLayout* rows_ = nullptr;
  QPushButton* update_all_ = nullptr;
  QLabel* empty_ = nullptr;
  QLabel* mira_status_ = nullptr;
  ProgressRail* mira_rail_ = nullptr;
  std::set<std::string> started_;  // rows whose request hasn't been answered, by kind and id
};

}  // namespace mira_gui
