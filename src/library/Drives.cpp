#include "library/Drives.h"

#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <json.hpp>
#include <map>
#include <set>
#include <sstream>

#include "config/Config.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "library/FolderTags.h"
#include "store/GameStore.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

constexpr std::uintmax_t kMinFree = 20ull * 1000 * 1000 * 1000;
constexpr const char* kSetUpKey = "drives_set_up";

// /proc/self/mounts writes a space in a path as \040.
std::string Unescape(const std::string& field) {
  std::string out;
  for (std::size_t i = 0; i < field.size(); ++i) {
    const std::string octal = field.substr(i + 1, 3);
    if (field[i] == '\\' && octal.size() == 3 &&
        octal.find_first_not_of("01234567") == std::string::npos) {
      out += static_cast<char>(std::stoi(octal, nullptr, 8));
      i += 3;
      continue;
    }
    out += field[i];
  }
  return out;
}

bool DiskType(const std::string& type) {
  static const std::set<std::string> kTypes = {"ext4",     "ext3",  "xfs",     "btrfs",
                                               "f2fs",     "ntfs3", "ntfs",    "exfat",
                                               "bcachefs", "jfs",   "fuseblk", "reiserfs"};
  return kTypes.contains(type);
}

// System places that are never a games drive even on a disk of their own.
bool SystemMount(const fs::path& path) {
  static const std::vector<fs::path> kSystem = {"/",     "/boot", "/efi", "/usr", "/var",
                                                "/nix",  "/opt",  "/srv", "/tmp", "/snap",
                                                "/root", "/proc", "/sys", "/dev", "/run/user"};
  return std::ranges::any_of(kSystem, [&](const fs::path& system) {
    return system == "/" ? path == "/" : IsAtOrUnder(path, system);
  });
}

// The device behind `path`'s filesystem, for comparing drives.
std::string DeviceOf(const std::vector<Mount>& mounts, const fs::path& path) {
  std::error_code ec;
  const fs::path real = fs::weakly_canonical(path, ec);
  const Mount* best = nullptr;
  for (const Mount& mount : mounts) {
    if (!IsAtOrUnder(real, mount.path)) continue;
    if (best == nullptr || mount.path.string().size() >= best->path.string().size()) best = &mount;
  }
  return best != nullptr ? best->device : std::string();
}

// A USB stick or other removable disk, from sysfs.
bool Removable(const std::string& device) {
  std::error_code ec;
  const fs::path block =
      fs::canonical(fs::path("/sys/class/block") / fs::path(device).filename(), ec);
  if (ec) return false;
  if (block.string().find("/usb") != std::string::npos) return true;
  for (fs::path at = block; at.has_relative_path() && at != "/sys"; at = at.parent_path()) {
    std::ifstream flag(at / "removable");
    int removable = 0;
    if (flag >> removable) return removable == 1;
  }
  return false;
}

std::string IdOf(const std::string& device) {
  std::error_code ec;
  for (const fs::directory_entry& entry : fs::directory_iterator("/dev/disk/by-uuid", ec)) {
    if (fs::canonical(entry.path(), ec) == fs::path(device)) return entry.path().filename();
  }
  return device;
}

}  // namespace

std::vector<Mount> ParseMounts(const std::string& text) {
  std::vector<Mount> mounts;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    std::istringstream fields(line);
    std::string device, path, type, options;
    if (!(fields >> device >> path >> type >> options)) continue;
    const std::string opts = "," + options + ",";
    mounts.push_back({.device = Unescape(device),
                      .path = Unescape(path),
                      .type = type,
                      .read_only = opts.find(",ro,") != std::string::npos});
  }
  return mounts;
}

std::vector<Drive> GameDrives() {
  std::ifstream file("/proc/self/mounts");
  std::stringstream text;
  text << file.rdbuf();
  const std::vector<Mount> mounts = ParseMounts(text.str());
  std::error_code ec;
  const std::set<std::string> boot = {DeviceOf(mounts, "/"), DeviceOf(mounts, paths::Home())};
  std::map<std::string, fs::path> by_device;  // a device mounted twice: its shortest path
  for (const Mount& mount : mounts) {
    if (!mount.device.starts_with("/dev/") || !DiskType(mount.type) || mount.read_only) continue;
    if (SystemMount(mount.path) || boot.contains(mount.device)) continue;
    const fs::path device = fs::canonical(mount.device, ec);
    if (ec || boot.contains(device.string()) || Removable(device.string())) continue;
    auto [it, added] = by_device.try_emplace(device.string(), mount.path);
    if (!added && mount.path.string().size() < it->second.string().size()) it->second = mount.path;
  }
  std::vector<Drive> drives;
  for (const auto& [device, path] : by_device) {
    const fs::space_info space = fs::space(path, ec);
    if (ec || space.available < kMinFree) continue;
    // A drive Mira can't write to, such as a root-owned /mnt/data.
    if (access(path.c_str(), W_OK) != 0) continue;
    drives.push_back({.id = IdOf(device), .mount = path});
  }
  return drives;
}

void SetUpDrives(config::Config& config, store::GameStore& games) {
  if (!config.GetBool("library.auto_drives")) return;
  nlohmann::json seen = nlohmann::json::parse(games.Secret(kSetUpKey), nullptr, false);
  if (!seen.is_array()) seen = nlohmann::json::array();
  std::vector<fs::path> folders = config.GetPathArray("mira_folders");
  std::vector<fs::path> roots = config.GetPathArray("library_roots");
  bool changed = false;
  for (const Drive& drive : GameDrives()) {
    if (std::ranges::contains(seen, nlohmann::json(drive.id))) continue;
    // Already holding a Mira folder the user added.
    if (std::ranges::any_of(folders,
                            [&](const fs::path& f) { return IsAtOrUnder(f, drive.mount); })) {
      seen.push_back(drive.id);
      continue;
    }
    const fs::path folder = drive.mount / "Mira";
    std::error_code ec;
    fs::create_directories(folder / "Games", ec);
    if (!ec) fs::create_directories(folder / "Applications", ec);
    if (ec) {
      log::Warn("couldn't set up {} for games: {}", folder.string(), ec.message());
      continue;  // tried again at the next start
    }
    seen.push_back(drive.id);
    log::Info("set up {} for games", folder.string());
    folders.push_back(folder);
    roots.push_back(folder / "Games");
    roots.push_back(folder / "Applications");
    changed = true;
  }
  if (auto saved = games.SetSecret(kSetUpKey, seen.dump()); !saved)
    log::Warn("couldn't remember the drives set up: {}", saved.error().message);
  if (!changed) return;
  const auto strings = [](const std::vector<fs::path>& list) {
    nlohmann::json out = nlohmann::json::array();
    for (const fs::path& path : list) out.push_back(path.string());
    return out;
  };
  if (auto saved =
          config.Patch({{"mira_folders", strings(folders)}, {"library_roots", strings(roots)}});
      !saved)
    log::Warn("couldn't add the new drives' Mira folders: {}", saved.error().message);
}

}  // namespace mira::library
