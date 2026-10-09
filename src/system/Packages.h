#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <json.hpp>

#include "core/Result.h"

// The system packages Mira's features need, by distribution: what's missing, and the command that
// installs it, which a client runs as root (pkexec).
namespace mira::system {

enum class Family { Unknown, Arch, Debian, Fedora, Suse, Ostree, SteamOS };

struct Distro {
  Family family = Family::Unknown;
  std::string id;    // os-release ID, e.g. "cachyos"
  std::string name;  // PRETTY_NAME
};

// From /etc/os-release (ID, then ID_LIKE). An ostree-booted system (Bazzite, Silverblue) is Ostree
// whatever its ID: packages are layered with rpm-ostree and take effect after a restart.
Distro DetectDistro();
Distro ParseOsRelease(std::string_view text, bool ostree_booted);

// One program (or library) a feature uses, and the package that has it on each family.
struct Tool {
  std::string_view id;
  std::string_view binary;  // on PATH; alternatives separated by spaces ("7z 7zz"); empty for a library
  std::string_view library;  // a shared library loaded at runtime ("libSDL3.so.0"), checked with dlopen
  std::string_view purpose;
  std::string_view arch, debian, fedora, suse;
};

// Every tool Mira knows, or those `feature` needs ("core", "wine", "winetricks", "archives", "performance",
// "controllers").
// Empty for an unknown feature.
std::vector<Tool> ToolsFor(std::string_view feature = {});

// The package for `tool` on `family`, alternatives separated by spaces; Ostree layers Fedora's.
std::string_view PackageFor(const Tool& tool, Family family);

// The command that installs `packages` as root, or empty where Mira can't (SteamOS, unknown).
std::vector<std::string> InstallCommand(Family family, const std::vector<std::string>& packages);

// The tools of `feature` not installed: {"distro", "family", "missing": [{tool, package, purpose}],
// "unavailable": [{tool, purpose}], "install": [argv...], "restart": bool}. `unavailable` has no
// package on this distro; `install` is empty when nothing's missing or Mira can't install them.
nlohmann::json Report(std::string_view feature = {});

// An error naming what's missing for `feature` and the command that installs it, with a fix a
// client turns into an install button; nothing when all of it is there.
Result<void> RequireFeature(std::string_view feature, std::string_view what);

}  // namespace mira::system
