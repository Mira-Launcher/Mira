#include "modmanagers/ModManagers.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <mutex>
#include <set>

#include "core/AtomicFile.h"
#include "core/Paths.h"
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
  return games.Update(game_id, [&](model::Game& g) {
    json options = Without(g.launch, manager.id);
    options.push_back({{"id", manager.id},
                       {"name", manager.name},
                       {"mode", "tool"},
                       {"exe", (Dir(manager) / manager.binary).string()},
                       {"args", ""},
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
