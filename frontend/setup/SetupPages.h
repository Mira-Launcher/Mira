#pragma once

#include <QHash>
#include <QPointer>
#include <QString>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "SetupWindow.h"

class QAbstractButton;
class QButtonGroup;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace mira_gui {

class ProgressRail;
class SignInPanel;
class Switch;

// Page 0: what Mira is, where it keeps games, and the system packages Next installs.
class WelcomePage : public SetupStep {
  Q_OBJECT

 public:
  explicit WelcomePage(const SetupContext& context);
  void Leave() override;

 private:
  void ShowFolder();
  void ChooseFolder(const QString& folder);
  void ShowPackages();

  const SetupContext& context_;
  QString folder_;
  SystemPackagesResult packages_;
  std::set<std::string> unticked_;  // missing packages left out of the install
  bool runners_known_ = false;
  bool windows_ready_ = false;  // a Proton or Wine build is installed

  QLabel* path_ = nullptr;
  ProgressRail* used_ = nullptr;
  QLabel* free_ = nullptr;
  QLabel* drive_warning_ = nullptr;
  QWidget* note_ = nullptr;
  QLabel* note_text_ = nullptr;
  QPushButton* choose_ = nullptr;
  QWidget* package_list_ = nullptr;
};

// What Mira is for: games, Windows applications, or both. Decides the pages that follow.
class UsePage : public SetupStep {
  Q_OBJECT

 public:
  explicit UsePage(const SetupContext& context);
  void Enter() override;

 private:
  const SetupContext& context_;
  QAbstractButton* games_ = nullptr;
  QAbstractButton* apps_ = nullptr;
};

// Steam and Lutris games already here, whose Steam account they are, and whether Windows games
// are ready to run.
class FoundPage : public SetupStep {
  Q_OBJECT

 public:
  explicit FoundPage(const SetupContext& context);
  void Enter() override;
  void Leave() override;

 private:
  void Show();
  int Count(const char* source) const;

  const SetupContext& context_;
  bool asked_ = false;
  SteamInstalledResult steam_;
  SteamAccountsResult accounts_;
  QString account_;  // the picked steamid64
  bool lutris_found_ = false;
  bool steam_on_ = true;
  bool lutris_on_ = true;
  std::vector<RunnerInfo> runners_;
  bool runners_known_ = false;
  GameModeStatusResult gamemode_;
  bool vulkan_ = true;

  QVBoxLayout* body_ = nullptr;
};

// Which stores the user buys games on. Each picked one gets a sign-in page.
class StoresPage : public SetupStep {
  Q_OBJECT

 public:
  explicit StoresPage(const SetupContext& context);
  void Enter() override;
  void Leave() override;

 private:
  void Show();

  const SetupContext& context_;
  std::map<QString, QAbstractButton*> tiles_;
  QLabel* count_ = nullptr;
};

// Which Windows applications to set up, and where apps show.
class AppsPage : public SetupStep {
  Q_OBJECT

 public:
  explicit AppsPage(const SetupContext& context);
  void Enter() override;
  void Leave() override;

 private:
  const SetupContext& context_;
  QAbstractButton* office_ = nullptr;
  QLabel* line_ = nullptr;
  Switch* in_all_ = nullptr;
  Switch* big_screen_ = nullptr;
};

// One store's sign-in, moving on by itself once it worked.
class StorePage : public SetupStep {
  Q_OBJECT

 public:
  StorePage(const SetupContext& context, const QString& store);
  void Enter() override;

 private:
  const SetupContext& context_;
  QString store_;
  QLabel* eyebrow_ = nullptr;
  QLabel* done_ = nullptr;
  SignInPanel* panel_ = nullptr;
};

// Microsoft 365's plan and apps. Next starts the install, after its Windows setup.
class OfficeSetupPage : public SetupStep {
  Q_OBJECT

 public:
  explicit OfficeSetupPage(const SetupContext& context);
  void Enter() override;
  void Leave() override;

 private:
  void ShowApps();

  const SetupContext& context_;
  std::vector<LauncherApp> apps_;
  QString plan_;
  QButtonGroup* plans_ = nullptr;
  QWidget* app_tiles_ = nullptr;
};

// What was set up and what's left, starting with the computer, and the way to GitHub.
class DonePage : public SetupStep {
  Q_OBJECT

 public:
  explicit DonePage(const SetupContext& context);
  void Enter() override;
  void Leave() override;
  // Open Mira in big screen.
  bool BigScreen() const;

 private:
  void ShowSummary();

  const SetupContext& context_;
  std::map<QString, bool> signed_in_;  // store -> signed in (Steam: its key saved)
  QLabel* title_ = nullptr;
  QWidget* summary_ = nullptr;
  Switch* login_ = nullptr;
  Switch* big_screen_ = nullptr;
};

}  // namespace mira_gui
