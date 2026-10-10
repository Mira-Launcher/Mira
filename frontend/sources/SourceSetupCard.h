#pragma once

#include <QPointer>
#include <QString>

#include "../client/Types.h"
#include "../settings/SettingsCard.h"
#include "Sources.h"

class QLabel;

namespace mira_gui {

// What a store or launcher still needs before its page is useful: the same panel Set up Mira
// shows (signing in step by step, or installing the launcher), made when first needed. Hidden
// once the source is set up.
class SourceSetupCard : public SettingsCard {
  Q_OBJECT

 public:
  SourceSetupCard(const SourceInfo& source, QWidget* parent);

  // A store's state as mirad last said.
  void ShowStore(bool tool_installed, bool authenticated);
  void ShowLauncher(const LauncherInfo& launcher, bool installing);
  // Shows the card with "<what> <mirad's error>".
  void ShowError(const QString& what, const ApiError& error);
  // The tool or launcher's setup job failed; `tool_installed` means it was an update.
  void ShowSetupFailed(const ApiError& error, bool tool_installed);

 signals:
  // Signed in or installed: the page asks mirad for the state again.
  void StatusChanged();
  void LauncherInstallStarted();
  void LauncherInstallFailed();

 private:
  // Shows the card while the source isn't set up, making its panel the first time.
  void ShowNeeded(bool needed);

  SourceInfo source_;
  QPointer<QWidget> panel_;  // a SignInPanel, or Microsoft 365's OfficePanel
  QLabel* error_ = nullptr;  // for errors once the panel is gone
};

}  // namespace mira_gui
