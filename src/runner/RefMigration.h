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
int RepointRefs(config::Config& config, store::GameStore& games, const MovedRef& moved,
                std::vector<std::string>* game_ids = nullptr);

// Builds updated in place (a distro package, Steam's own Proton) are named by
// their folder. References written before that named the release inside
// ("proton:cachyos-11.0-20260703-slr"), which stops existing on the next
// update; this points them at the folder's build, when exactly one fits
// (same major version, unless the folder isn't versioned). Covers games' runner_ref,
// their overrides and every runner setting. Returns how many it changed.
int MigrateInPlaceRefs(config::Config& config, store::GameStore& games);

// Points every reference to a build that isn't installed anymore at the build that runs in its
// place (RunnerRegistry::StandIn), so what a game shows is what it runs. Returns how many it
// changed, adding the games' ids to `game_ids`.
int RepointRemovedBuilds(config::Config& config, store::GameStore& games,
                         std::vector<std::string>* game_ids = nullptr);

}  // namespace mira::runner
