#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "store/GameStore.h"

// A source's runner: a store's default (<store>.runner) or the runner of a
// launcher's shared prefix.
namespace mira::library {

struct SourceRunner {
  std::string runner_ref;            // empty: fall back to the default
  int games = 0;                     // the source's Windows games
  int differing = 0;                 // of those, the ones on another runner
  std::vector<std::string> changed;  // SetSourceRunner only: ids of the games it moved
};

// 400 no_runner for sources without one (Steam, Lutris, Humble); 409
// launcher_not_installed for a launcher with no prefix yet.
Result<SourceRunner> GetSourceRunner(const config::Config& config, const store::GameStore& games,
                                     std::string_view source);

// A store's default applies to its games with no runner of their own, and
// to all of them when `apply_to_games`. A launcher's prefix is shared, so
// the launcher and its games always change together. Returns the new state.
Result<SourceRunner> SetSourceRunner(config::Config& config, store::GameStore& games,
                                     std::string_view source, const std::string& runner_ref,
                                     bool apply_to_games);

}  // namespace mira::library
