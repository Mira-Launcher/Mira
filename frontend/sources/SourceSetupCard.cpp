#include "SourceSetupCard.h"

#include <QLabel>
#include <QVBoxLayout>

#include "../setup/OfficePanel.h"
#include "../setup/SignInPanel.h"
#include "../widgets/Labels.h"
#include "SourceText.h"

namespace mira_gui {

SourceSetupCard::SourceSetupCard(const SourceInfo& source, QWidget* parent)
    : SettingsCard("Set up " + source.name, parent), source_(source) {
  setVisible(false);
  error_ = MakeLabel(this, QString(), "error");
  error_->setContentsMargins(18, 4, 18, 14);
  error_->setVisible(false);
  AddRow(error_);
}

void SourceSetupCard::ShowNeeded(bool needed) {
  if (!needed) {
    // Gone with the setup, so signing out later starts it fresh.
    if (panel_ != nullptr) RemoveRow(panel_);
    panel_ = nullptr;
    setVisible(false);
    return;
  }
  error_->setVisible(false);
  setVisible(true);
  if (panel_ != nullptr) return;
  if (source_.id == "office") {
    auto* panel = new OfficePanel(this, /*header=*/false);
    connect(panel, &OfficePanel::Changed, this, &SourceSetupCard::StatusChanged);
    connect(panel, &OfficePanel::InstallStarted, this, &SourceSetupCard::LauncherInstallStarted);
    connect(panel, &OfficePanel::InstallFailed, this, &SourceSetupCard::LauncherInstallFailed);
    panel_ = panel;
  } else {
    auto* panel = new SignInPanel(source_.id, this, /*header=*/false);
    connect(panel, &SignInPanel::Connected, this, &SourceSetupCard::StatusChanged);
    connect(panel, &SignInPanel::InstallStarted, this, &SourceSetupCard::LauncherInstallStarted);
    connect(panel, &SignInPanel::InstallFailed, this, &SourceSetupCard::LauncherInstallFailed);
    panel_ = panel;
  }
  panel_->setContentsMargins(18, 4, 18, 14);
  AddRow(panel_);
}

void SourceSetupCard::ShowStore(bool /*tool_installed*/, bool authenticated) {
  ShowNeeded(!authenticated);
}

void SourceSetupCard::ShowLauncher(const LauncherInfo& launcher, bool /*installing*/) {
  ShowNeeded(!launcher.installed);
}

void SourceSetupCard::ShowError(const QString& what, const ApiError& error) {
  setVisible(true);
  if (auto* panel = qobject_cast<SignInPanel*>(panel_.data())) return panel->ShowFailure(what, error);
  if (auto* panel = qobject_cast<OfficePanel*>(panel_.data())) return panel->ShowFailure(what, error);
  mira_gui::ShowError(error_, what, error);
}

void SourceSetupCard::ShowSetupFailed(const ApiError& error, bool tool_installed) {
  // A first install's failure is the panel's to show; an update happens once it's gone.
  if (tool_installed) ShowError("Updating " + CopyFor(source_.id.toStdString()).tool + " failed.", error);
}

}  // namespace mira_gui
