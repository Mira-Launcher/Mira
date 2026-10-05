#include "library/SourceRemoval.h"

#include <algorithm>
#include <filesystem>
#include <format>

#include "amazon/Nile.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "epic/Legendary.h"
#include "gog/Gog.h"
#include "itch/Butlerd.h"
#include "itch/Itch.h"
#include "launchers/Launchers.h"
#include "library/Stores.h"
#include "metadata/MetadataFetcher.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

bool IsStore(std::string_view source) {
  return source == "epic" || source == "gog" || source == "itch" || source == "amazon";
}

// Only the app owns these games' files.
bool FilesOwnedElsewhere(std::string_view source) {
  return source == "steam" || source == "lutris" || source == "humble";
}

bool Belongs(const model::Game& game, std::string_view source,
             const launchers::Launcher* launcher) {
  if (game.source == source) return true;
  return launcher != nullptr && game.id == launchers::GameId(*launcher);
}

std::vector<fs::path> DeleteRoots(const config::Config& config, const std::string& prefix) {
  std::vector<fs::path> roots = config.GetPathArray("library_roots");
  for (const char* key : {"gog.install_root", "itch.install_root", "amazon.install_root"}) {
    roots.push_back(config.GetPath(key));
  }
  // A launcher game installed inside its prefix.
  if (!prefix.empty()) roots.push_back(fs::path(prefix) / "drive_c");
  return roots;
}

std::string WhatGetsDeleted(const model::Game& game, std::string_view source) {
  if (FilesOwnedElsewhere(source) || game.install_path.empty()) return {};
  if (source == "epic") return "legendary uninstall " + game.source_ref;
  if (source == "amazon") return "nile uninstall " + game.source_ref;
  if (source == "itch") return "butler uninstall (" + game.install_path + ")";
  return game.install_path;
}

Result<void> UninstallItch(const config::Config& config, const std::string& game_ref) {
  const Result<std::int64_t> profile_id = itch::CurrentProfileId(config);
  if (!profile_id) return std::unexpected(profile_id.error());
  const Result<json> caves = itch::Call(config, "Fetch.Caves", {{"profileId", *profile_id}});
  if (!caves) return std::unexpected(caves.error());
  for (const json& cave : caves->value("items", json::array())) {
    if (std::to_string(cave.value("game", json::object()).value("id", std::int64_t{0})) != game_ref)
      continue;
    if (auto done = itch::CallLong(config, "Uninstall.Perform",
                                   {{"caveId", cave.value("id", std::string())}});
        !done) {
      return std::unexpected(done.error());
    }
  }
  return {};
}

Result<void> UninstallGame(const config::Config& config, const model::Game& game,
                           std::string_view source) {
  if (FilesOwnedElsewhere(source) || game.install_path.empty()) return {};
  if (source == "epic") {
    if (auto done = epic::RunLegendary(config, {"uninstall", game.source_ref, "-y"}); !done) {
      return std::unexpected(done.error());
    }
    return {};
  }
  if (source == "amazon") {
    if (auto done = amazon::RunNile(config, {"uninstall", game.source_ref}); !done)
      return std::unexpected(done.error());
    return {};
  }
  if (source == "itch") return UninstallItch(config, game.source_ref);
  return DeleteInside(game.install_path, DeleteRoots(config, game.data_dir));
}

// Deletes everything in `dir` except the `keep` entries (by name).
Result<void> DeleteKeeping(const fs::path& dir, const std::vector<std::string>& keep,
                           const fs::path& prefix) {
  std::error_code ec;
  if (!fs::exists(dir, ec)) return {};
  for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
    if (std::ranges::contains(keep, entry.path().filename().string())) continue;
    if (auto deleted = DeleteInside(entry.path().string(), {prefix / "drive_c"}); !deleted)
      return deleted;
  }
  if (keep.empty() || fs::is_empty(dir, ec))
    return DeleteInside(dir.string(), {prefix / "drive_c"});
  return {};
}

// A launcher's program folder, relative to drive_c. EA keeps versioned
// folders one level up from its exe.
fs::path ProgramDir(const launchers::Launcher& launcher) {
  const fs::path dir = fs::path(launcher.exe).parent_path();
  return launcher.id == "ea" ? dir.parent_path() : dir;
}

// Save folders some launchers keep inside their own program folder.
std::vector<std::string> KeptInProgramDir(std::string_view launcher) {
  if (launcher == "ubisoft") return {"savegames"};
  return {};
}

}  // namespace

Result<void> DeleteInside(const std::string& target, const std::vector<fs::path>& roots) {
  if (target.empty()) return Err("nothing_to_delete", "no path recorded");
  std::error_code ec;
  const fs::path resolved = fs::weakly_canonical(target, ec);
  if (ec) return Err("path_error", ec.message());
  if (!paths::IsWithin(resolved, roots))
    return Err("path_outside_root",
               std::format("\"{}\" isn't inside a Mira folder, so it was left alone", target));
  fs::remove_all(resolved, ec);
  if (ec) return Err("delete_failed", ec.message());
  return {};
}

Result<RemovalPlan> PlanRemoval(const config::Config& /*config*/, const store::GameStore& games,
                                std::string_view source) {
  const launchers::Launcher* launcher = launchers::Find(source);
  if (!launcher && !IsStore(source) && !FilesOwnedElsewhere(source)) {
    return Err("unknown_source", std::format("no source named \"{}\"", source));
  }
  RemovalPlan plan;
  plan.source = std::string(source);
  plan.signs_out = IsStore(source);
  for (const model::Game& game : games.All()) {
    if (!Belongs(game, source, launcher)) continue;
    if (!game.data_dir.empty() && !std::ranges::contains(plan.kept, game.data_dir))
      plan.kept.push_back(game.data_dir);
    if (launcher != nullptr && game.id == launchers::GameId(*launcher)) {
      plan.launcher_dir = (fs::path(game.data_dir) / "drive_c" / ProgramDir(*launcher)).string();
      for (const std::string& keep : KeptInProgramDir(source)) {
        plan.kept.push_back((fs::path(plan.launcher_dir) / keep).string());
      }
      continue;
    }
    plan.games.push_back(
        {.id = game.id, .name = game.name, .deletes = WhatGetsDeleted(game, source)});
  }
  return plan;
}

Result<RemovalResult> RemoveSource(config::Config& config, store::GameStore& games,
                                   api::EventBus& events, std::string_view source) {
  const Result<RemovalPlan> plan = PlanRemoval(config, games, source);
  if (!plan) return std::unexpected(plan.error());
  const launchers::Launcher* launcher = launchers::Find(source);

  RemovalResult result;
  const auto forget = [&](const std::string& id) {
    std::error_code ec;
    fs::remove(metadata::MetadataFile(config, id), ec);
    fs::remove_all(metadata::ArtworkDir(config, id), ec);
    if (games.Remove(id)) {
      events.Publish("game.removed", {{"id", id}});
      ++result.removed;
    }
  };

  for (const RemovalGame& planned : plan->games) {
    const auto folders_lock = games.LockFolders();
    const std::optional<model::Game> game = games.Find(planned.id);
    if (!game) continue;
    if (auto done = UninstallGame(config, *game, source); !done) {
      // Files stay; the record goes anyway so the source can still be removed.
      result.problems.push_back(std::format("{}: {}", game->name, done.error().message));
    }
    forget(game->id);
  }

  if (launcher != nullptr) {
    if (const std::optional<model::Game> entry = games.Find(launchers::GameId(*launcher))) {
      const fs::path program = fs::path(entry->data_dir) / "drive_c" / ProgramDir(*launcher);
      if (auto done = DeleteKeeping(program, KeptInProgramDir(source), entry->data_dir); !done) {
        result.problems.push_back(std::format("{}: {}", launcher->name, done.error().message));
      }
      forget(entry->id);
    }
  }

  if (plan->signs_out) {
    const Store* store = FindStore(source);
    const Result<void> signed_out = store && store->logout ? store->logout(config) : Result<void>();
    if (!signed_out) result.problems.push_back("signing out: " + signed_out.error().message);
  }

  if (auto off = config.Set(std::format("{}.enabled", source), false); !off) {
    result.problems.push_back("turning it off: " + off.error().message);
  }
  log::Info("removed source {}: {} game(s), {} problem(s)", source, result.removed,
            result.problems.size());
  return result;
}

}  // namespace mira::library
