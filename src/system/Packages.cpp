#include "system/Packages.h"

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

#include "runner/Exec.h"

namespace mira::system {
namespace {

using nlohmann::json;

// Feature, tool, binary, purpose, then the package on Arch, Debian/Ubuntu, Fedora and openSUSE.
struct Entry {
  std::string_view feature;
  Tool tool;
};

constexpr Entry kEntries[] = {
    {"winetricks", {"cabextract", "cabextract", "Unpacks Windows fonts and libraries for winetricks",
                    "cabextract", "cabextract", "cabextract", "cabextract"}},
    {"winetricks", {"unzip", "unzip", "Unpacks downloads for winetricks", "unzip", "unzip", "unzip", "unzip"}},
    {"archives", {"7zip", "7z", "Extracts game archives and installers", "7zip", "7zip", "7zip", "7zip"}},
    {"performance", {"gamemode", "gamemoded", "Raises performance while a game runs", "gamemode", "gamemode",
                     "gamemode", "gamemode"}},
    {"performance", {"mangohud", "mangohud", "Shows an FPS and performance overlay", "mangohud", "mangohud",
                     "mangohud", "mangohud"}},
};

std::string_view FamilyName(Family family) {
  switch (family) {
    case Family::Arch: return "arch";
    case Family::Debian: return "debian";
    case Family::Fedora: return "fedora";
    case Family::Suse: return "suse";
    case Family::Ostree: return "ostree";
    case Family::SteamOS: return "steamos";
    case Family::Unknown: break;
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
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) value = value.substr(1, value.size() - 2);
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
      if (id == "arch") distro.family = Family::Arch;
      else if (id == "debian" || id == "ubuntu") distro.family = Family::Debian;
      else if (id == "fedora" || id == "rhel") distro.family = Family::Fedora;
      else if (id.starts_with("opensuse") || id == "suse") distro.family = Family::Suse;
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
    if (feature.empty() || entry.feature == feature) tools.push_back(entry.tool);
  }
  return tools;
}

std::string_view PackageFor(const Tool& tool, Family family) {
  switch (family) {
    case Family::Arch: return tool.arch;
    case Family::Debian: return tool.debian;
    case Family::Fedora:
    case Family::Ostree: return tool.fedora;
    case Family::Suse: return tool.suse;
    case Family::SteamOS:
    case Family::Unknown: break;
  }
  return tool.id;
}

std::vector<std::string> InstallCommand(Family family, const std::vector<std::string>& packages) {
  if (packages.empty()) return {};
  std::vector<std::string> argv;
  switch (family) {
    case Family::Arch: argv = {"pacman", "-S", "--needed", "--noconfirm"}; break;
    case Family::Debian: argv = {"apt-get", "install", "-y"}; break;
    case Family::Fedora: argv = {"dnf", "install", "-y"}; break;
    case Family::Suse: argv = {"zypper", "--non-interactive", "install"}; break;
    case Family::Ostree: argv = {"rpm-ostree", "install", "--idempotent", "--allow-inactive"}; break;
    case Family::SteamOS:
    case Family::Unknown: return {};
  }
  argv.insert(argv.end(), packages.begin(), packages.end());
  return argv;
}

json Report(std::string_view feature) {
  const Distro distro = DetectDistro();
  json missing = json::array();
  std::vector<std::string> packages;
  for (const Tool& tool : ToolsFor(feature)) {
    if (runner::FindOnPath(tool.binary)) continue;
    const std::string package(PackageFor(tool, distro.family));
    missing.push_back({{"tool", tool.id}, {"package", package}, {"purpose", tool.purpose}});
    packages.push_back(package);
  }
  return {{"distro", distro.name.empty() ? distro.id : distro.name},
          {"family", FamilyName(distro.family)},
          {"missing", std::move(missing)},
          {"install", InstallCommand(distro.family, packages)},
          {"restart", distro.family == Family::Ostree && !packages.empty()}};
}

Result<void> RequireFeature(std::string_view feature, std::string_view what) {
  const json report = Report(feature);
  if (report["missing"].empty()) return {};
  std::vector<std::string> packages;
  for (const json& item : report["missing"]) packages.push_back(item["package"].get<std::string>());
  const std::vector<std::string> install = report["install"].get<std::vector<std::string>>();
  const std::string hint = install.empty() ? std::format("Install {} with your system's package manager.", Join(packages, " and "))
                                           : std::format("Install it with: sudo {}", Join(install, " "));
  return Err("missing_packages", std::format("{} needs {}, which isn't installed", what, Join(packages, " and ")), hint,
             Fix{"packages", std::string(feature), {}});
}

}  // namespace mira::system
