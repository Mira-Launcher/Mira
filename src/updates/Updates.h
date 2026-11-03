#pragma once

#include <filesystem>
#include <functional>
#include <json.hpp>
#include <string>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "system/Packages.h"

// Newer releases of Mira and of what it downloaded: emulators, store tools, umu and runner builds.
namespace mira::updates {

struct Update {
  std::string kind;  // "mira", "emulator", "tool" or "runner"
  std::string id;    // the emulator's id, the store's ("gog"), "umu", or the runner's reference
  std::string name;  // "DuckStation"
  std::string installed;  // the tag or version there now
  std::string latest;
  std::string url;           // Mira's release page
  std::string install = {};  // Mira's: see ClassifyInstall
};

// Asks each source at once; one that can't be reached is left out. `refresh` skips what's cached.
std::vector<Update> FindUpdates(config::Config& config, bool refresh = false);

// How this copy of Mira updates: "appimage", "deb" or "rpm" by itself, "manual" for a package it
// can't (Arch, rpm-ostree), "none" for a build of the user's own. `succeeds` runs a command.
std::string ClassifyInstall(const std::string& appimage, const std::filesystem::path& exe,
                            system::Family family,
                            const std::function<bool(const std::vector<std::string>&)>& succeeds);

// Downloads and verifies Mira's latest release. An AppImage is swapped in place, for the next start
// to run; `{"install": [argv...]}` is the command that installs a package as root, empty for an
// AppImage.
Result<nlohmann::json> DownloadMira(
    const std::function<void(int percent, const std::string& message)>& on_progress);

// Removes a package left by DownloadMira, once it's installed or given up on.
void RemoveDownloadedMira();

}  // namespace mira::updates
