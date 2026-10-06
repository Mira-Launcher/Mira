#pragma once

#include <QString>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "../settings/SettingsCard.h"
#include "Sources.h"

class QLabel;
class QTimer;
class QLineEdit;
class QPushButton;

namespace mira_gui {

// What a store or launcher still needs before its page is useful, as
// numbered steps: get the store's helper tool, then sign in; or install the
// launcher. Then import. The current step holds its text, its button or
// sign-in row, and the last error. Hidden once everything is set up.
class SourceSetupCard : public SettingsCard {
  Q_OBJECT

 public:
  SourceSetupCard(const SourceInfo& source, QWidget* parent);

  // A store's state as mirad last said.
  void ShowStore(bool tool_installed, bool authenticated);
  void ShowLauncher(const LauncherInfo& launcher, bool installing);
  // Shows the card with "<what> <mirad's error>" under the current step.
  void ShowError(const QString& what, const ApiError& error);
  // The tool or launcher's setup job failed; `tool_installed` means it was an update.
  void ShowSetupFailed(const ApiError& error, bool tool_installed);

 signals:
  // A download or sign-in finished: the page asks mirad for the state again.
  void StatusChanged();
  void LauncherInstallStarted();
  void LauncherInstallFailed();

 private:
  bool IsLauncher() const { return source_.kind == SourceInfo::Kind::Launcher; }
  void StartSetup();
  // Marks steps before `current` done and shows the body under it.
  void SetStep(int current);
  void OpenLogin();
  // The running log under the step: the last few lines, refreshed while setup runs.
  void WatchLog(bool on);
  void PollLog();
  void SignIn();

  struct Step {
    QWidget* row = nullptr;
    QLabel* marker = nullptr;
    QLabel* title = nullptr;
  };

  SourceInfo source_;
  std::string id_;
  std::vector<Step> steps_;
  QWidget* body_ = nullptr;
  QLabel* text_ = nullptr;
  QPushButton* button_ = nullptr;  // download the tool / install the launcher
  QWidget* sign_in_row_ = nullptr;
  QPushButton* open_login_ = nullptr;
  QLineEdit* credential_ = nullptr;
  QPushButton* sign_in_ = nullptr;
  QLabel* error_ = nullptr;
  QWidget* log_box_ = nullptr;
  QLabel* log_tail_ = nullptr;
  QTimer* log_timer_ = nullptr;
  bool log_busy_ = false;
};

}  // namespace mira_gui
