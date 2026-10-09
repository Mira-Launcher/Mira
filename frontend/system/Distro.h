#pragma once

#include <QString>
#include <QStringList>

namespace mira_gui::system {

struct Distro {
  enum class Family { Unknown, Arch, Debian, Fedora, Suse, Ostree, SteamOS };
  Family family = Family::Unknown;
  QString id;    // os-release ID, e.g. "cachyos"
  QString name;  // os-release PRETTY_NAME
};

// From /etc/os-release (ID, then ID_LIKE). An ostree-booted system (Silverblue, Bazzite) is Ostree
// whatever its ID, since its package manager can't change the running system.
Distro DetectDistro();

// The command that upgrades the whole system, to run as root (through pkexec); empty when unknown.
QStringList UpgradeCommand(const Distro& distro);

// After an upgrade whose output was `output`: whether a restart is needed to use it.
bool NeedsRestart(const Distro& distro, const QString& output);

}  // namespace mira_gui::system
