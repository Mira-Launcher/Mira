#include "modmanagers/ModManagers.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <mutex>
#include <set>

#include "core/AtomicFile.h"
#include "core/Paths.h"
#include "launchers/Launchers.h"
#include "library/WinePrefix.h"
#include "runner/Curl.h"
#include "runner/Downloader.h"
#include "runner/Exec.h"
#include "runner/RunnerRegistry.h"
#include "runner/Winetricks.h"

namespace mira::modmanagers {
namespace fs = std::filesystem;
using nlohmann::json;
namespace {

constexpr const char* kVersionFile = ".mira-version";

// A game's launch options without the one named `id`.
json Without(const json& launch, const std::string& id) {
  json kept = json::array();
  for (const json& option : launch.value("options", json::array())) {
    if (!option.is_object() || option.value("id", std::string()) != id) kept.push_back(option);
  }
  return kept;
}

constexpr const char* kMo2Ini = "ModOrganizer.ini";

bool SamePath(const fs::path& a, const fs::path& b) {
  if (a.empty() || b.empty()) return false;
  std::error_code ec;
  const fs::path left = fs::weakly_canonical(a, ec);
  return !ec && left == fs::weakly_canonical(b, ec) && !ec;
}

// `path` as programs in `prefix` see it: under C: inside drive_c, else through Z:.
std::string WindowsPath(const fs::path& prefix, const fs::path& path) {
  std::error_code ec;
  const fs::path drive_c = fs::weakly_canonical(prefix / "drive_c", ec);
  const fs::path target = fs::weakly_canonical(path, ec);
  std::string out = paths::IsWithin(target, {drive_c})
                        ? "C:\\" + target.lexically_relative(drive_c).string()
                        : "Z:" + target.string();
  std::ranges::replace(out, '/', '\\');
  return out;
}

// An MO2 instance's gamePath as written: by MO2 (@ByteArray(...)) or by Mira (quoted).
std::string Mo2GamePath(const fs::path& ini) {
  std::ifstream in(ini);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.starts_with("gamePath=")) continue;
    std::string_view value = std::string_view(line).substr(std::string_view("gamePath=").size());
    if (value.starts_with("@ByteArray(") && value.ends_with(')'))
      value = value.substr(11, value.size() - 12);  // @ByteArray( and )
    else if (value.size() >= 2 && value.starts_with('"') && value.ends_with('"'))
      value = value.substr(1, value.size() - 2);
    // Qt writes a backslash as two.
    std::string path;
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\') ++i;
      path.push_back(value[i]);
    }
    return path;
  }
  return {};
}

// The game's name as a Windows folder name, its id when nothing of it is left.
std::string InstanceName(const model::Game& game) {
  std::string name;
  for (const char c : game.name) {
    if (static_cast<unsigned char>(c) >= 32 && !std::string_view(R"(<>:"/\|?*)").contains(c))
      name.push_back(c);
  }
  while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
  while (!name.empty() && name.front() == ' ') name.erase(0, 1);
  return name.empty() ? game.id : name;
}

// The name of the game's MO2 instance, made with only its folder when it has none: opening it,
// MO2 finds the game from the folder and fills in the rest.
Result<std::string> EnsureMo2Instance(const model::Game& game) {
  const fs::path dir = Mo2InstancesDir(game);
  for (const fs::path& instance : paths::ListDir(dir)) {
    if (Mo2Manages(instance / kMo2Ini, game)) return instance.filename().string();
  }
  std::string name = InstanceName(game);
  std::error_code ec;
  if (fs::exists(dir / name, ec)) name += " (" + game.id + ")";
  std::string path = WindowsPath(library::PrefixRoot(game.data_dir), game.install_path);
  std::string escaped;
  for (const char c : path) escaped += c == '\\' ? std::string("\\\\") : std::string(1, c);
  // Quoted, so a comma in the path doesn't make it a list.
  fs::create_directories(dir / name, ec);
  if (auto written = WriteFileAtomic(dir / name / kMo2Ini,
                                     std::format("[General]\ngamePath=\"{}\"\n", escaped),
                                     "mod_manager_failed");
      !written)
    return std::unexpected(written.error());
  return name;
}

// Unpacks `archive` into `dir`, which is made empty first.
Result<void> ExtractFresh(const fs::path& archive, const fs::path& dir) {
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  if (ec)
    return Err("install_failed", std::format("couldn't make {}: {}", dir.string(), ec.message()));
  return runner::Extract(archive, dir);
}

}  // namespace

const std::vector<ModManager>& All() {
  static const std::vector<ModManager> all = {
      {.id = "vortex",
       .name = "Vortex",
       .release_url = "https://api.github.com/repos/Nexus-Mods/Vortex/releases/latest",
       .asset = "vortex-setup-*.exe",
       .inner = "$PLUGINSDIR/app-64.7z",
       .binary = "Vortex.exe",
       .tricks = {"vcrun2022", "dotnetdesktop9"}},
      {.id = "mo2",
       .name = "Mod Organizer 2",
       .release_url = "https://api.github.com/repos/ModOrganizer2/modorganizer/releases/latest",
       .asset = "Mod.Organizer-*.7z",
       .exclude = {"-pdbs", "-src", "-uibase"},
       .binary = "ModOrganizer.exe",
       .tricks = {"vcrun2022"}},
  };
  return all;
}

const ModManager* Find(std::string_view id) {
  const auto it = std::ranges::find(All(), id, &ModManager::id);
  return it == All().end() ? nullptr : &*it;
}

fs::path Dir(const ModManager& manager) {
  return paths::UserDir() / "tools" / manager.id;
}

bool Installed(const ModManager& manager) {
  std::error_code ec;
  return fs::is_regular_file(Dir(manager) / manager.binary, ec);
}

std::string InstalledVersion(const ModManager& manager) {
  std::ifstream file(Dir(manager) / kVersionFile);
  std::string tag;
  std::getline(file, tag);
  return Installed(manager) ? tag : std::string();
}

namespace {
// The managers with an install or uninstall running; one at a time for each.
std::mutex jobs_mutex;
std::set<std::string> jobs;

bool ClaimJob(const std::string& id) {
  std::lock_guard lock(jobs_mutex);
  return jobs.insert(id).second;
}

struct JobSlot {
  const std::string& id;
  ~JobSlot() {
    std::lock_guard lock(jobs_mutex);
    jobs.erase(id);
  }
};
}  // namespace

Result<void> Install(const ModManager& manager) {
  if (!ClaimJob(manager.id))
    return Err("already_installing", std::format("{} is already being installed", manager.name));
  const JobSlot job{manager.id};

  const auto asset = runner::LatestRelease(manager.release_url, manager.asset, manager.exclude);
  if (!asset) return std::unexpected(asset.error());
  // Unpacked beside its folder and swapped in, so a failed download leaves the old copy working.
  const fs::path dir = Dir(manager);
  const fs::path staging = dir.string() + ".new";
  const fs::path download = dir.string() + ".download";
  std::error_code ec;
  fs::create_directories(dir.parent_path(), ec);
  const auto cleanup = [&] {
    fs::remove_all(staging, ec);
    fs::remove_all(download, ec);
  };
  fs::create_directories(download, ec);
  const fs::path file = download / asset->asset_name;
  if (auto got = runner::CurlDownload(asset->download_url, file); !got) {
    cleanup();
    return got;
  }
  Result<void> unpacked = ExtractFresh(file, staging);
  if (unpacked && !manager.inner.empty()) {
    const fs::path inner = download / "inner";
    fs::rename(staging / manager.inner, inner, ec);
    unpacked = ec ? Err("install_failed",
                        std::format("{}'s download has no {}", manager.name, manager.inner))
                  : ExtractFresh(inner, staging);
  }
  if (unpacked && !fs::is_regular_file(staging / manager.binary, ec)) {
    unpacked =
        Err("install_failed", std::format("{}'s download has no {}", manager.name, manager.binary));
  }
  if (!unpacked) {
    cleanup();
    return unpacked;
  }
  if (auto written = WriteFileAtomic(staging / kVersionFile, asset->tag + '\n', "install_failed");
      !written) {
    cleanup();
    return written;
  }
  // Renames only, so the old copy stays whole until the new one is in place.
  const fs::path old = dir.string() + ".old";
  fs::remove_all(old, ec);
  fs::rename(dir, old, ec);
  ec.clear();
  fs::rename(staging, dir, ec);
  if (ec) {
    const std::string why = ec.message();
    fs::rename(old, dir, ec);
    cleanup();
    return Err("install_failed", std::format("couldn't move {} into place: {}", manager.name, why));
  }
  fs::remove_all(old, ec);
  cleanup();
  return {};
}

bool CanAdd(const model::Game& game) {
  return game.platform == model::Platform::Windows && !game.data_dir.empty();
}

bool OnGame(const model::Game& game, const ModManager& manager) {
  const json options = game.launch.value("options", json::array());
  return std::ranges::any_of(options, [&](const json& option) {
    return option.is_object() && option.value("id", std::string()) == manager.id;
  });
}

fs::path Mo2InstancesDir(const model::Game& game) {
  if (game.data_dir.empty()) return {};
  return library::WindowsUserDir(library::PrefixRoot(game.data_dir)) / "AppData/Local/ModOrganizer";
}

bool Mo2Manages(const fs::path& ini, const model::Game& game) {
  if (game.data_dir.empty() || game.install_path.empty()) return false;
  const std::string path = Mo2GamePath(ini);
  return !path.empty() &&
         SamePath(launchers::HostPath(library::PrefixRoot(game.data_dir), path), game.install_path);
}

Error OpenError(const ModManager& manager, std::string_view game_name) {
  return {"mod_manager_open", std::format("{} is open for {}", manager.name, game_name),
          std::format("Close {}, then remove it again.", manager.name)};
}

Result<model::Game> AddToGame(config::Config& config, store::GameStore& games,
                              const ModManager& manager, const std::string& game_id) {
  const auto game = games.Find(game_id);
  if (!game) return Err("game_not_found", "no such game");
  if (!CanAdd(*game)) {
    return Err("not_windows_game",
               std::format("{} runs in a Windows game's prefix, and {} has none", manager.name,
                           game->name));
  }
  if (!Installed(manager)) {
    return Err("mod_manager_missing", std::format("{} isn't installed", manager.name),
               std::format("Add {} from Sources first.", manager.name));
  }
  const runner::RunnerRegistry runners(config);
  for (const std::string& verb : manager.tricks) {
    if (auto ran = runner::RunTricksVerb(runners, *game, verb); !ran)
      return std::unexpected(ran.error());
  }
  std::string args;
  if (manager.id == "mo2" && !game->install_path.empty()) {
    const auto instance = EnsureMo2Instance(*game);
    if (!instance) return std::unexpected(instance.error());
    args = std::format("-i \"{}\"", *instance);
  }
  return games.Update(game_id, [&](model::Game& g) {
    json options = Without(g.launch, manager.id);
    options.push_back({{"id", manager.id},
                       {"name", manager.name},
                       {"mode", "tool"},
                       {"exe", (Dir(manager) / manager.binary).string()},
                       {"args", args},
                       {"native", false}});
    g.launch["options"] = std::move(options);
  });
}

Result<model::Game> RemoveFromGame(store::GameStore& games, const ModManager& manager,
                                   const std::string& game_id) {
  return games.Update(game_id,
                      [&](model::Game& g) { g.launch["options"] = Without(g.launch, manager.id); });
}

Result<void> Uninstall(store::GameStore& games, const ModManager& manager) {
  if (!ClaimJob(manager.id))
    return Err("busy", std::format("{} is busy with another install or uninstall", manager.name));
  const JobSlot job{manager.id};
  for (const model::Game& game : games.All()) {
    if (!OnGame(game, manager)) continue;
    if (auto removed = RemoveFromGame(games, manager, game.id); !removed)
      return std::unexpected(removed.error());
  }
  std::error_code ec;
  fs::remove_all(Dir(manager), ec);
  if (ec)
    return Err("uninstall_failed",
               std::format("couldn't delete {}: {}", Dir(manager).string(), ec.message()));
  return {};
}

}  // namespace mira::modmanagers
