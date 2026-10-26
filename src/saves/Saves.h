#pragma once

#include <json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "config/Config.h"
#include "model/Types.h"
#include "saves/Match.h"
#include "saves/Resolve.h"
#include "store/GameStore.h"
#include "store/MetadataStore.h"

namespace mira::saves {

// What Mira does with a game's saves, decided before looking at its files.
struct Plan {
  bool backup = false;
  // Why not, when not: "off" (saves.enabled), "steam_cloud" (Steam starts the game and its Steam
  // Cloud keeps the saves, while saves.skip_cloud is on).
  std::string status{};
};
Plan PlanFor(const config::Config& config, const model::Game& game,
             const std::optional<Match>& match);

// A game's save locations and the files in them now: the list entry's that apply to it,
// saves.extra_paths, then its store's cloud save folders as last asked.
struct Survey {
  std::optional<Match> match;
  Plan plan{};
  std::vector<Location> locations{};
  bool registry = false;  // the entry also lists registry saves
};
Survey SurveyFor(const config::Config& config, const store::MetadataStore& cache,
                 const model::Game& game);

// GET /v1/games/{id}/saves's answer: {enabled, status, entry, match, locations, snapshots,
// last_error?, cloud}. Status "kept" (snapshots taken and the game is backed up), "ready" (files
// found), "no_files", "no_entry" (no save locations known for it), "registry_only", or a Plan's.
nlohmann::json Overview(const config::Config& config, const store::MetadataStore& cache,
                        const store::GameStore& games, const model::Game& game);

}  // namespace mira::saves
