#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "config/Config.h"
#include "store/GameStore.h"

namespace mira::runner {

// The reference a runner reference moves to, or nothing when it stays.
using MovedRef = std::function<std::optional<std::string>(const std::string& ref)>;

// Moves every runner reference `moved` maps: games' runner_ref, their overrides and every runner
// setting. Returns how many it changed, adding the games' ids to `game_ids`.
// With `moves_game`, only the games it picks move.
int RepointRefs(config::Config& config, store::GameStore& games, const MovedRef& moved,
                std::vector<std::string>* game_ids = nullptr,
                const std::function<bool(const model::Game&)>& moves_game = {});

// Points every reference to a build that isn't installed anymore at the build that runs in its
// place (RunnerRegistry::StandIn), so what a game shows is what it runs. Returns how many it
// changed, adding the games' ids to `game_ids`.
int RepointRemovedBuilds(config::Config& config, store::GameStore& games,
                         std::vector<std::string>* game_ids = nullptr);

}  // namespace mira::runner
