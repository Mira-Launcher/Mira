#pragma once

#include <QString>

#include <functional>
#include <string>
#include <vector>

class QWidget;

namespace mira_gui::system {

// Runs `install` (mirad's command for `names`) as root: through pkexec, where the desktop asks for the
// password; without pkexec, or when it fails, shows the command to copy or open in a terminal. `done(true)`
// once installed; false when it failed, was dismissed, went to a terminal, or needs a restart first
// (rpm-ostree).
void InstallPackages(QWidget* parent, const std::vector<std::string>& install, const QString& names, bool restart,
                     std::function<void(bool installed)> done);

// Makes sure the system packages `feature` needs (GET /v1/system/packages) are installed before
// `what` ("Microsoft 365") goes ahead: shows the command, then InstallPackages. `done(true)` once nothing's missing.
void EnsurePackages(QWidget* parent, const std::string& feature, const QString& what,
                    std::function<void(bool ready)> done);

// "a, b and c".
QString Names(const std::vector<std::string>& packages);

}  // namespace mira_gui::system
