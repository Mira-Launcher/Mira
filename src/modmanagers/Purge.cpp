#include "modmanagers/Purge.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "core/Paths.h"
#include "launchers/Launchers.h"
#include "library/WinePrefix.h"

namespace mira::modmanagers {
namespace fs = std::filesystem;
using nlohmann::json;
namespace {

constexpr std::string_view kBackup = ".vortex_backup";
// Files Vortex leaves in folders it made, so it knows to delete them once empty.
constexpr std::array<std::string_view, 2> kMarkers = {"__folder_managed_by_vortex",
                                                      "__delete_if_empty"};
// In every staging folder Vortex made; a staging folder without it isn't deleted.
constexpr const char* kStagingMarker = "__vortex_staging_folder";
constexpr const char* kPurgeHint =
    "Close any program that has the game's files open, then remove it again.";

bool IsManifest(const fs::path& file) {
  const std::string name = file.filename().string();
  return name.starts_with("vortex.deployment") && name.ends_with(".json");
}

// One of Vortex's deployment records: which of its staged files it put where.
struct Manifest {
  std::string name;  // its file name, one per mod type
  std::string game_id;
  fs::path target;   // where the files went
  fs::path staging;  // where they came from
  json files;
};

// `windows` with Windows' separators, relative.
fs::path Relative(std::string windows) {
  std::ranges::replace(windows, '\\', '/');
  return fs::path(windows).relative_path();
}

std::optional<Manifest> ReadManifest(const fs::path& prefix, const fs::path& file) {
  std::ifstream in(file);
  const json body = json::parse(in, nullptr, /*allow_exceptions=*/false);
  if (!body.is_object()) return std::nullopt;
  Manifest manifest{
      .name = file.filename().string(),
      .game_id = body.value("gameId", std::string()),
      .target = launchers::HostPath(prefix, body.value("targetPath", std::string())),
      .staging = launchers::HostPath(prefix, body.value("stagingPath", std::string())),
      .files = body.value("files", json::array())};
  if (manifest.game_id.empty() || manifest.target.empty() || manifest.staging.empty())
    return std::nullopt;
  return manifest;
}

// A hard link to its staged copy, or a symlink into the staging folder: Vortex's, not the game's.
bool IsDeployed(const fs::path& file, const fs::path& staged, const fs::path& staging) {
  std::error_code ec;
  const fs::file_status status = fs::symlink_status(file, ec);
  if (ec) return false;
  if (fs::is_symlink(status)) return paths::IsWithin(file, {staging});
  if (!fs::is_regular_file(status) || fs::hard_link_count(file, ec) < 2 || ec) return false;
  return fs::equivalent(file, staged, ec) && !ec;
}

bool SamePath(const fs::path& a, const fs::path& b) {
  if (a.empty() || b.empty()) return false;
  std::error_code ec;
  const fs::path left = fs::weakly_canonical(a, ec);
  return !ec && left == fs::weakly_canonical(b, ec) && !ec;
}

void AddExisting(std::vector<fs::path>& to, const fs::path& path) {
  std::error_code ec;
  if (fs::exists(path, ec) && !std::ranges::contains(to, path)) to.push_back(path);
}

void PlanVortex(PurgePlan& plan, const model::Game& game, const fs::path& prefix,
                const fs::path& vortex, bool shared) {
  const fs::path install = game.install_path;
  std::vector<Manifest> found;
  const auto read = [&](const fs::path& file) {
    if (auto manifest = ReadManifest(prefix, file)) found.push_back(std::move(*manifest));
  };
  // In the folders it deployed to, which for most games are the game's folder or one just inside.
  std::error_code ec;
  if (!install.empty()) {
    for (fs::recursive_directory_iterator
             it(install, fs::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
      if (it.depth() >= 2) it.disable_recursion_pending();
      if (IsManifest(it->path()) && it->is_regular_file(ec)) read(it->path());
    }
  }
  // And a copy in each staging folder: <game>/mods by default.
  for (const fs::path& dir : paths::ListDir(vortex)) {
    for (const fs::path& file : paths::ListDir(dir / "mods")) {
      if (IsManifest(file)) read(file);
    }
  }

  // Vortex's ids for this game: those that deploy into its folder. Their other targets (a
  // Documents folder) go too.
  std::set<std::string> ids;
  for (const Manifest& manifest : found) {
    if (paths::IsWithin(manifest.target, {install}, /*allow_equal=*/true))
      ids.insert(manifest.game_id);
  }
  std::set<std::pair<std::string, std::string>> done;
  std::set<fs::path> walked;
  std::set<fs::path> stagings;
  for (const Manifest& manifest : found) {
    if (!ids.contains(manifest.game_id)) continue;
    if (!done.emplace(manifest.target.lexically_normal().string(), manifest.name).second) continue;
    for (const json& entry : manifest.files) {
      if (!entry.is_object()) continue;
      const fs::path rel = Relative(entry.value("relPath", std::string()));
      if (rel.empty()) continue;
      const fs::path file = manifest.target / rel;
      const fs::path staged =
          manifest.staging / Relative(entry.value("source", std::string())) / rel;
      if (IsDeployed(file, staged, manifest.staging)) plan.deployed.push_back(file);
    }
    AddExisting(plan.manifests, manifest.target / manifest.name);
    // Its markers and set-aside files, wherever they are under the target (one target can hold
    // another).
    if (walked.insert(manifest.target.lexically_normal()).second) {
      for (fs::recursive_directory_iterator
               it(manifest.target, fs::directory_options::skip_permission_denied, ec),
           end;
           !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (std::ranges::contains(kMarkers, name)) {
          if (!std::ranges::contains(plan.emptied, it->path().parent_path()))
            plan.emptied.push_back(it->path().parent_path());
        } else if (name.ends_with(kBackup) && it->is_regular_file(ec) &&
                   !std::ranges::contains(plan.restored, it->path(),
                                          &std::pair<fs::path, fs::path>::first)) {
          plan.restored.emplace_back(
              it->path(), it->path().parent_path() / name.substr(0, name.size() - kBackup.size()));
        }
      }
    }
    if (std::error_code marker; fs::is_regular_file(manifest.staging / kStagingMarker, marker) &&
                                !paths::IsWithin(install, {manifest.staging}, true))
      stagings.insert(manifest.staging.lexically_normal());
  }
  // A game file that's still there stays; its set-aside copy is left beside it.
  std::erase_if(plan.restored, [&](const auto& restore) {
    return fs::exists(restore.second, ec) && !std::ranges::contains(plan.deployed, restore.second);
  });

  if (!shared) {
    AddExisting(plan.deleted, vortex);
  } else {
    for (const std::string& id : ids) {
      AddExisting(plan.deleted, vortex / id);
      AddExisting(plan.deleted, vortex / "downloads" / id);
    }
  }
  // A staging folder moved elsewhere in Vortex's settings.
  for (const fs::path& staging : stagings) {
    if (!paths::IsWithin(staging, plan.deleted, /*allow_equal=*/true))
      AddExisting(plan.deleted, staging);
  }
}

void PlanMo2(PurgePlan& plan, const ModManager& manager, const model::Game& game) {
  const fs::path instances = Mo2InstancesDir(game);
  for (const fs::path& dir : paths::ListDir(instances)) {
    if (Mo2Manages(dir / "ModOrganizer.ini", game)) plan.deleted.push_back(dir);
  }
  if (!plan.deleted.empty()) plan.emptied.push_back(instances);
  // A portable instance keeps its data beside the program, in Mira's copy every game shares.
  const fs::path own = Dir(manager);
  if (Mo2Manages(own / "ModOrganizer.ini", game)) {
    for (const char* name : {"ModOrganizer.ini", "mods", "downloads", "profiles", "overwrite",
                             "webcache", "logs", "crashDumps"})
      AddExisting(plan.deleted, own / name);
  }
}

}  // namespace

PurgePlan PlanPurge(const ModManager& manager, const model::Game& game,
                    std::span<const model::Game> games) {
  PurgePlan plan;
  if (game.data_dir.empty()) return plan;
  const fs::path prefix = library::PrefixRoot(game.data_dir);
  if (manager.id == "vortex") {
    const bool shared = std::ranges::any_of(games, [&](const model::Game& other) {
      return other.id != game.id && !other.data_dir.empty() &&
             SamePath(library::PrefixRoot(other.data_dir), prefix);
    });
    PlanVortex(plan, game, prefix, library::WindowsUserDir(prefix) / "AppData/Roaming/Vortex",
               shared);
  } else if (manager.id == "mo2") {
    PlanMo2(plan, manager, game);
  }
  // Deepest first, so a folder whose only content was an emptied one goes too.
  std::ranges::sort(plan.emptied, [](const fs::path& a, const fs::path& b) {
    return a.string().size() > b.string().size();
  });
  return plan;
}

json PurgeSummary(const PurgePlan& plan) {
  json deleted = json::array();
  std::uintmax_t total = 0;
  for (const fs::path& path : plan.deleted) {
    const std::uintmax_t bytes = paths::TreeBytes(path);
    total += bytes;
    deleted.push_back({{"path", path.string()}, {"bytes", bytes}});
  }
  return {{"deployed", plan.deployed.size()},
          {"restored", plan.restored.size()},
          {"deleted", std::move(deleted)},
          {"bytes", total}};
}

Result<void> Purge(const PurgePlan& plan) {
  std::error_code ec;
  const auto failed = [&ec](std::string_view what, const fs::path& path) {
    return Err("purge_failed", std::format("couldn't {} {}: {}", what, path.string(), ec.message()),
               kPurgeHint);
  };
  for (const fs::path& file : plan.deployed) {
    if (fs::remove(file, ec); ec) return failed("delete", file);
  }
  for (const auto& [backup, original] : plan.restored) {
    if (fs::exists(original, ec)) continue;
    if (fs::rename(backup, original, ec); ec) return failed("put back", original);
  }
  for (const fs::path& file : plan.manifests) {
    if (fs::remove(file, ec); ec) return failed("delete", file);
  }
  for (const fs::path& path : plan.deleted) {
    if (fs::remove_all(path, ec); ec) return failed("delete", path);
  }
  for (const fs::path& dir : plan.emptied) {
    const std::vector<fs::path> left = paths::ListDir(dir);
    if (!std::ranges::all_of(left, [](const fs::path& entry) {
          return std::ranges::contains(kMarkers, entry.filename().string());
        }))
      continue;
    for (const fs::path& marker : left) fs::remove(marker, ec);
    fs::remove(dir, ec);
  }
  return {};
}

Result<model::Game> PurgeFromGame(store::GameStore& games, const ModManager& manager,
                                  const std::string& game_id) {
  const auto game = games.Find(game_id);
  if (!game) return Err("game_not_found", "no such game");
  if (auto purged = Purge(PlanPurge(manager, *game, games.All())); !purged)
    return std::unexpected(purged.error());
  return RemoveFromGame(games, manager, game_id);
}

}  // namespace mira::modmanagers
