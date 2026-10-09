#pragma once

#include <QString>

#include <functional>
#include <string>

class QWidget;

namespace mira_gui::system {

// Makes sure the system packages `feature` needs (GET /v1/system/packages) are installed before
// `what` ("Microsoft 365") goes ahead: asks, then installs the missing ones through pkexec, where
// the desktop asks for the password. `done(true)` once nothing's missing; false when the user
// declined, it failed, a restart is needed first (rpm-ostree), or they must install it by hand.
void EnsurePackages(QWidget* parent, const std::string& feature, const QString& what,
                    std::function<void(bool ready)> done);

}  // namespace mira_gui::system
