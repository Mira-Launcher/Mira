#pragma once

#include <cstdint>
#include <filesystem>
#include <json.hpp>
#include <span>
#include <utility>
#include <vector>

#include "core/Result.h"
#include "model/Types.h"
#include "modmanagers/ModManagers.h"

// Undoing what a mod manager did for one game, in that game's prefix: what Vortex deployed into
// the game's folders comes out, the game files it set aside go back, and the manager's mods,
// downloads and settings for the game are deleted.
namespace mira::modmanagers {

struct PurgePlan {
  // Files Vortex deployed: hard links to its staged copies, or symlinks into its staging folder.
  std::vector<std::filesystem::path> deployed;
  // Game files Vortex set aside (*.vortex_backup), each with where it goes back.
  std::vector<std::pair<std::filesystem::path, std::filesystem::path>> restored;
  // Vortex's records of what it deployed (vortex.deployment*.json) in the game's folders.
  std::vector<std::filesystem::path> manifests;
  // Folders deleted once nothing but Vortex's markers is left in them, deepest first.
  std::vector<std::filesystem::path> emptied;
  // The manager's own data: Vortex's staging folders and downloads, an MO2 instance.
  std::vector<std::filesystem::path> deleted;
  // Keys taken out of the prefix's user.reg (`registry`), as written there: the nxm: link handler
  // it registered and MO2's settings, unless another game in the prefix still has it.
  std::filesystem::path registry;
  std::vector<std::string> keys;
  std::string data_dir;  // the game's, for what runs in its prefix
};

// What Purge would do for `manager` on `game`, read from the prefix as it is now. `games` is the
// whole library: in a prefix another game shares, only this game's part of Vortex's folder goes.
PurgePlan PlanPurge(const ModManager& manager, const model::Game& game,
                    std::span<const model::Game> games);
// {deployed, restored, deleted: [{path, bytes}], bytes}: counts, and each folder deleted with its
// size.
nlohmann::json PurgeSummary(const PurgePlan& plan);
// Carries the plan out, stopping at the first file that can't be deleted or put back.
Result<void> Purge(const PurgePlan& plan);
// Purges `manager` from the game, then takes it off; a failed purge leaves it on. The caller holds
// the game's claim and has checked the manager isn't open.
Result<model::Game> PurgeFromGame(store::GameStore& games, const ModManager& manager,
                                  const std::string& game_id);

}  // namespace mira::modmanagers
