#pragma once

#include <QPointer>
#include <QSet>
#include <QString>
#include <QWidget>
#include <string>
#include <vector>

#include "../client/Types.h"

class QButtonGroup;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace mira_gui {

class ProgressRail;
class SetupLog;

// launchers.office.plan's values, by the plan names people know them by.
struct OfficePlan {
  QString value;
  QString name;
  QString short_name;  // where the choice says it's a plan already
};
const std::vector<OfficePlan>& OfficePlans();

// Sets up Microsoft 365: which plan the subscription is (the edition Office installs), the install
// into its own prefix, then which of its apps go into the library. Each app installs on its own.
class OfficePanel : public QWidget {
  Q_OBJECT

 public:
  // Without `header` there's no Back, title or done line, for a card that has its own.
  explicit OfficePanel(QWidget* parent, bool header = true);

  void Refresh();
  bool Installed() const { return installed_; }
  // Shows "<what> <mirad's error>" at the bottom.
  void ShowFailure(const QString& what, const ApiError& error);

 signals:
  void BackClicked();
  void Changed();
  void InstallStarted();
  void InstallFailed();

 private:
  void ShowState();
  void Install();
  void AddApps();
  void InstallNext();
  void HandleEvent(const std::string& type, const std::string& data);

  bool installed_ = false;
  bool installing_ = false;
  double progress_ = -1;
  std::vector<StoreTitle> apps_;
  QSet<QString> picked_;
  std::vector<std::string> queue_;  // apps still to install, one at a time

  QLabel* done_label_ = nullptr;
  QWidget* plan_box_ = nullptr;
  QButtonGroup* plans_ = nullptr;
  QPushButton* install_ = nullptr;
  QWidget* progress_box_ = nullptr;
  QLabel* progress_text_ = nullptr;
  ProgressRail* rail_ = nullptr;
  QWidget* apps_box_ = nullptr;
  QWidget* app_tiles_ = nullptr;
  QPushButton* add_ = nullptr;
  QLabel* error_ = nullptr;
  SetupLog* log_ = nullptr;
};

}  // namespace mira_gui
