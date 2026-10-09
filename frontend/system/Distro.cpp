#include "Distro.h"

#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSysInfo>

namespace mira_gui::system {

Distro DetectDistro() {
  Distro distro;
  QStringList like;
  QFile file("/etc/os-release");
  if (!file.exists()) file.setFileName("/usr/lib/os-release");
  if (file.open(QIODevice::ReadOnly)) {
    for (const QString& line : QString::fromUtf8(file.readAll()).split('\n')) {
      const qsizetype eq = line.indexOf('=');
      if (eq < 0) continue;
      QString value = line.mid(eq + 1).trimmed();
      if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) value = value.mid(1, value.size() - 2);
      const QString key = line.left(eq);
      if (key == "ID") distro.id = value;
      if (key == "ID_LIKE") like = value.split(' ', Qt::SkipEmptyParts);
      if (key == "PRETTY_NAME") distro.name = value;
    }
  }
  like.prepend(distro.id);
  using F = Distro::Family;
  if (distro.id == "steamos") {
    distro.family = F::SteamOS;
  } else if (QFileInfo::exists("/run/ostree-booted")) {
    distro.family = F::Ostree;
  } else {
    for (const QString& id : like) {
      if (id == "arch") distro.family = F::Arch;
      else if (id == "debian" || id == "ubuntu") distro.family = F::Debian;
      else if (id == "fedora" || id == "rhel") distro.family = F::Fedora;
      else if (id.startsWith("opensuse") || id == "suse") distro.family = F::Suse;
      if (distro.family != F::Unknown) break;
    }
  }
  return distro;
}

QStringList UpgradeCommand(const Distro& distro) {
  using F = Distro::Family;
  switch (distro.family) {
    case F::Arch: return {"pacman", "-Syu", "--noconfirm"};
    case F::Debian:
      // Changed config files keep the user's copy instead of stopping to ask.
      return {"sh", "-c",
              "apt-get update && DEBIAN_FRONTEND=noninteractive apt-get -y -o Dpkg::Options::=--force-confdef "
              "-o Dpkg::Options::=--force-confold full-upgrade"};
    case F::Fedora: return {"dnf", "upgrade", "-y", "--refresh"};
    // Tumbleweed moves forward with dup; Leap stays on its release with update.
    case F::Suse:
      return distro.id.contains("tumbleweed") ? QStringList{"zypper", "--non-interactive", "dup"}
                                              : QStringList{"zypper", "--non-interactive", "update"};
    case F::Ostree:
      if (!QStandardPaths::findExecutable("bootc").isEmpty()) return {"bootc", "upgrade"};
      return {"rpm-ostree", "upgrade"};
    case F::SteamOS: return {"steamos-update"};
    case F::Unknown: break;
  }
  return {};
}

bool NeedsRestart(const Distro& distro, const QString& output) {
  using F = Distro::Family;
  // Image-based systems stage the update for the next boot.
  if (distro.family == F::Ostree || distro.family == F::SteamOS) {
    return !output.contains("No upgrade available") && !output.contains("No changes") &&
           !output.contains("No update available");
  }
  if (QFileInfo::exists("/run/reboot-required") || QFileInfo::exists("/var/run/reboot-required")) return true;
  // The running kernel's modules are gone once a new kernel replaced them.
  const QString kernel = QSysInfo::kernelVersion();
  return !QFileInfo::exists("/usr/lib/modules/" + kernel) && !QFileInfo::exists("/lib/modules/" + kernel);
}

}  // namespace mira_gui::system
