#pragma once

#include <QString>

#include <functional>
#include <string>
#include <vector>

class QWidget;

namespace mira_gui::system {

// Runs `install` (mirad's command for `names`) as root: through pkexec, where the desktop asks for the
// password; without pkexec, in a terminal with sudo; without either, shows the command to copy. `done(true)`
// once installed; false when it failed, was dismissed, went to a terminal or the user, or needs a restart
// first (rpm-ostree).
void InstallPackages(QWidget* parent, const std::vector<std::string>& install, const QString& names, bool restart,
                     std::function<void(bool installed)> done);

// Makes sure the system packages `feature` needs (GET /v1/system/packages) are installed before
// `what` ("Microsoft 365") goes ahead: asks, then InstallPackages. `done(true)` once nothing's missing.
void EnsurePackages(QWidget* parent, const std::string& feature, const QString& what,
                    std::function<void(bool ready)> done);

// "a, b and c".
QString Names(const std::vector<std::string>& packages);

}  // namespace mira_gui::system
