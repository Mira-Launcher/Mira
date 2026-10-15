#include "system/Packages.h"

#include <dlfcn.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>

#include "runner/Exec.h"

namespace mira::system {
namespace {

using nlohmann::json;

// Feature, tool, binary, purpose, then the package on Arch, Debian/Ubuntu, Fedora and openSUSE.
struct Entry {
  std::string_view feature;
  Tool tool;
  bool on_demand = false;  // an emulator's: asked for when it's set up, never in the full list
};

constexpr Entry kEntries[] = {
    {"core",
     {"curl", "curl", "", "Downloads covers, store info and Proton builds", "curl", "curl", "curl",
      "curl"}},
    {"wine",
     {"wine", "wine", "", "Runs Windows programs without a Proton build", "wine", "wine", "wine",
      "wine"}},
    {"wine",
     {"winetricks", "winetricks", "", "Installs Windows libraries and fixes into a game's prefix",
      "winetricks", "winetricks", "winetricks", "winetricks"}},
    {"winetricks",
     {"cabextract", "cabextract", "", "Unpacks Windows fonts and libraries for winetricks",
      "cabextract", "cabextract", "cabextract", "cabextract"}},
    {"winetricks",
     {"unzip", "unzip", "", "Unpacks downloads for winetricks", "unzip", "unzip", "unzip",
      "unzip"}},
    {"adobe",
     {"zstd", "zstd", "", "Unpacks AdobeWine, which Adobe Creative Cloud runs on", "zstd", "zstd",
      "zstd", "zstd"}},
    {"dolphin",
     {"dolphin-emu", "dolphin-emu", "", "Plays GameCube and Wii games", "dolphin-emu",
      "dolphin-emu", "dolphin-emu", "dolphin-emu"},
     true},
    {"ares",
     {"ares", "ares", "", "Plays Nintendo, Sega, NEC, SNK and Bandai games", "ares-emu", "", "",
      ""},
     true},
    {"snes9x",
     {"snes9x-gtk", "snes9x-gtk", "", "Plays SNES games", "snes9x-gtk", "", "snes9x-gtk",
      "snes9x-gtk"},
     true},
    {"mame", {"mame", "mame", "", "Plays arcade games", "mame", "mame", "mame", "mame"}, true},
    {"scummvm",
     {"scummvm", "scummvm", "", "Plays classic adventure games", "scummvm", "scummvm", "scummvm",
      "scummvm"},
     true},
    {"mednafen",
     {"mednafen", "mednafen", "",
      "Plays Saturn, Virtual Boy, Neo Geo Pocket, WonderSwan and Lynx games", "mednafen",
      "mednafen", "mednafen", "mednafen"},
     true},
    {"mupen64plus",
     {"mupen64plus", "mupen64plus", "", "Plays Nintendo 64 games", "mupen64plus",
      "mupen64plus-ui-console", "mupen64plus", "mupen64plus"},
     true},
    {"hatari",
     {"hatari", "hatari", "", "Plays Atari ST games", "hatari", "hatari", "hatari", "hatari"},
     true},
    {"vice",
     {"vice", "x64sc", "", "Plays Commodore 64 games", "vice", "vice", "vice", "vice"},
     true},
    {"stella",
     {"stella", "stella", "", "Plays Atari 2600 games", "stella", "stella", "stella", "stella"},
     true},
    {"bsnes",
     {"libao", "", "libao.so.4", "Plays bsnes's sound", "libao", "libao4", "libao", "libao4"},
     true},
    {"archives",
     {"7zip", "7z 7zz", "", "Extracts game archives and installers", "7zip", "7zip p7zip-full",
      "7zip", "7zip"}},
    {"performance",
     {"gamemode", "gamemoded", "", "Raises performance while a game runs", "gamemode", "gamemode",
      "gamemode", "gamemode"}},
    {"performance",
     {"mangohud", "mangohud", "", "Shows an FPS and performance overlay", "mangohud", "mangohud",
      "mangohud", "mangohud"}},
    {"controllers",
     {"sdl3", "", "libSDL3.so.0", "Controllers and sounds in big screen", "sdl3", "libsdl3-0",
      "SDL3", "libSDL3-0"}},
};

bool Installed(const Tool& tool) {
  if (!tool.library.empty()) {
    void* handle = ::dlopen(std::string(tool.library).c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (handle != nullptr) ::dlclose(handle);
    return handle != nullptr;
  }
  std::istringstream names{std::string(tool.binary)};
  for (std::string name; names >> name;) {
    if (runner::FindOnPath(name)) return true;
  }
  return false;
}

// The first of `alternatives` (space-separated) the distro has: apt rejects the whole install for
// one unknown name, and Debian and Ubuntu releases name some differently. Other families take the
// first. nullopt when apt couldn't answer for some name, so "not found" isn't reported for a slow
// apt.
std::optional<std::string> Choose(std::string_view alternatives, Family family) {
  std::istringstream names{std::string(alternatives)};
  bool unchecked = false;
  for (std::string name; names >> name;) {
    if (family != Family::Debian) return name;
    Command show;
    show.argv = {"apt-cache", "show", "--quiet=2", name};
    show.timeout_s = 10;
    const auto shown = runner::RunAndWait(show);
    if (shown && shown->exit_code == 0) return name;
    // Exit 100 is apt's "no such package".
    if (!shown || shown->exit_code != 100) unchecked = true;
  }
  if (unchecked) return std::nullopt;
  return std::string{};
}

std::string_view FamilyName(Family family) {
  switch (family) {
    case Family::Arch:
      return "arch";
    case Family::Debian:
      return "debian";
    case Family::Fedora:
      return "fedora";
    case Family::Suse:
      return "suse";
    case Family::Ostree:
      return "ostree";
    case Family::SteamOS:
      return "steamos";
    case Family::Unknown:
      break;
  }
  return "unknown";
}

std::string Join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string out;
  for (const std::string& part : parts) out += (out.empty() ? "" : std::string(separator)) + part;
  return out;
}

}  // namespace

Distro ParseOsRelease(std::string_view text, bool ostree_booted) {
  Distro distro;
  std::vector<std::string> like;
  std::istringstream lines{std::string(text)};
  for (std::string line; std::getline(lines, line);) {
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string value = line.substr(eq + 1);
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\''))
      value = value.substr(1, value.size() - 2);
    const std::string key = line.substr(0, eq);
    if (key == "ID") distro.id = value;
    if (key == "PRETTY_NAME") distro.name = value;
    if (key == "ID_LIKE") {
      std::istringstream ids(value);
      for (std::string id; ids >> id;) like.push_back(id);
    }
  }
  like.insert(like.begin(), distro.id);
  if (distro.id == "steamos") {
    distro.family = Family::SteamOS;
  } else if (ostree_booted) {
    distro.family = Family::Ostree;
  } else {
    for (const std::string& id : like) {
      if (id == "arch")
        distro.family = Family::Arch;
      else if (id == "debian" || id == "ubuntu")
        distro.family = Family::Debian;
      else if (id == "fedora" || id == "rhel")
        distro.family = Family::Fedora;
      else if (id.starts_with("opensuse") || id == "suse")
        distro.family = Family::Suse;
      if (distro.family != Family::Unknown) break;
    }
  }
  return distro;
}

Distro DetectDistro() {
  std::ifstream file("/etc/os-release");
  if (!file) file.open("/usr/lib/os-release");
  std::stringstream text;
  text << file.rdbuf();
  std::error_code ec;
  return ParseOsRelease(text.str(), std::filesystem::exists("/run/ostree-booted", ec));
}

std::vector<Tool> ToolsFor(std::string_view feature) {
  std::vector<Tool> tools;
  for (const Entry& entry : kEntries) {
    if (feature.empty() ? !entry.on_demand : entry.feature == feature) tools.push_back(entry.tool);
  }
  return tools;
}

std::string_view PackageFor(const Tool& tool, Family family) {
  switch (family) {
    case Family::Arch:
      return tool.arch;
    case Family::Debian:
      return tool.debian;
    case Family::Fedora:
    case Family::Ostree:
      return tool.fedora;
    case Family::Suse:
      return tool.suse;
    case Family::SteamOS:
    case Family::Unknown:
      break;
  }
  return tool.id;
}

std::vector<std::string> InstallCommand(Family family, const std::vector<std::string>& packages) {
  if (packages.empty()) return {};
  std::vector<std::string> argv;
  switch (family) {
    case Family::Arch:
      argv = {"pacman", "-S", "--needed", "--noconfirm"};
      break;
    case Family::Debian:
      argv = {"apt-get", "install", "-y"};
      break;
    case Family::Fedora:
      argv = {"dnf", "install", "-y"};
      break;
    case Family::Suse:
      argv = {"zypper", "--non-interactive", "install"};
      break;
    case Family::Ostree:
      argv = {"rpm-ostree", "install", "--idempotent", "--allow-inactive"};
      break;
    case Family::SteamOS:
    case Family::Unknown:
      return {};
  }
  argv.insert(argv.end(), packages.begin(), packages.end());
  return argv;
}

std::vector<std::string> InstalledPackages(std::string_view feature) {
  const Family family = DetectDistro().family;
  std::vector<std::string> packages;
  for (const Tool& tool : ToolsFor(feature)) {
    if (tool.binary.empty() || !Installed(tool)) continue;
    if (auto package = Choose(PackageFor(tool, family), family); package && !package->empty())
      packages.push_back(std::move(*package));
  }
  return packages;
}

std::vector<std::string> UninstallCommand(Family family, const std::vector<std::string>& packages) {
  if (packages.empty()) return {};
  std::vector<std::string> argv;
  switch (family) {
    case Family::Arch:
      argv = {"pacman", "-Rns", "--noconfirm"};
      break;
    case Family::Debian:
      argv = {"apt-get", "remove", "-y"};
      break;
    case Family::Fedora:
      argv = {"dnf", "remove", "-y"};
      break;
    case Family::Suse:
      argv = {"zypper", "--non-interactive", "remove"};
      break;
    case Family::Ostree:
      argv = {"rpm-ostree", "uninstall"};
      break;
    case Family::SteamOS:
    case Family::Unknown:
      return {};
  }
  argv.insert(argv.end(), packages.begin(), packages.end());
  return argv;
}

json Report(std::string_view feature) {
  const Distro distro = DetectDistro();
  json missing = json::array();
  json unavailable = json::array();  // no package on this distro
  std::vector<std::string> packages;
  for (const Tool& tool : ToolsFor(feature)) {
    if (Installed(tool)) continue;
    const auto chosen = Choose(PackageFor(tool, distro.family), distro.family);
    const std::string package = chosen.value_or("");
    if (package.empty()) {
      unavailable.push_back({{"tool", tool.id}, {"purpose", tool.purpose}, {"unchecked", !chosen}});
      continue;
    }
    missing.push_back({{"tool", tool.id}, {"package", package}, {"purpose", tool.purpose}});
    packages.push_back(package);
  }
  return {{"distro", distro.name.empty() ? distro.id : distro.name},
          {"family", FamilyName(distro.family)},
          {"missing", std::move(missing)},
          {"unavailable", std::move(unavailable)},
          {"install", InstallCommand(distro.family, packages)},
          {"restart", distro.family == Family::Ostree && !packages.empty()}};
}

Result<void> RequireFeature(std::string_view feature, std::string_view what) {
  const json report = Report(feature);
  if (report["missing"].empty() && report["unavailable"].empty()) return {};
  if (report["missing"].empty()) {
    // Missing with no package here: nothing to install, but it still won't run without it.
    std::vector<std::string> tools;
    bool unchecked = false;
    for (const json& item : report["unavailable"]) {
      tools.push_back(std::format("{} ({})", item["tool"].get<std::string>(),
                                  item["purpose"].get<std::string>()));
      if (item["unchecked"].get<bool>()) unchecked = true;
    }
    const std::string hint =
        unchecked ? "Couldn't check apt for a package. Try again, or install it another way."
                  : std::format("{} has no package for it that Mira knows; install it another way.",
                                report["distro"].get<std::string>());
    return Err("missing_tools",
               std::format("{} needs {}, which isn't installed", what, Join(tools, " and ")), hint);
  }
  std::vector<std::string> packages;
  for (const json& item : report["missing"]) packages.push_back(item["package"].get<std::string>());
  const std::vector<std::string> install = report["install"].get<std::vector<std::string>>();
  const std::string hint =
      install.empty()
          ? std::format("Install {} with your system's package manager.", Join(packages, " and "))
          : std::format("Install it with: sudo {}", Join(install, " "));
  return Err("missing_packages",
             std::format("{} needs {}, which isn't installed", what, Join(packages, " and ")), hint,
             Fix{"packages", std::string(feature), {}});
}

}  // namespace mira::system
