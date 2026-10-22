#pragma once

#include <json.hpp>
#include <optional>
#include <string>

#include "config/Config.h"
#include "model/Types.h"
#include "saves/Match.h"
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

// GET /v1/games/{id}/saves's answer: {enabled, status, entry, match, locations}. Status "ready"
// (files found), "no_files", "no_entry" (no save locations known for it), "registry_only", or a
// Plan's.
nlohmann::json Overview(const config::Config& config, const store::MetadataStore& cache,
                        const model::Game& game);

}  // namespace mira::saves
