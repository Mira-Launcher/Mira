#pragma once

#include <string>

#include "achievements/Achievements.h"
#include "config/Config.h"
#include "core/Result.h"

namespace mira::epic {

// An Epic game's achievements and the account's unlocks, through `legendary achievements`
// (legendary 0.21.1 or newer), in Epic's own order from legendary's metadata cache. Legendary
// picks the language from the system locale itself.
Result<achievements::GameAchievements> FetchAchievements(const config::Config& config,
                                                         const std::string& app_name);

}  // namespace mira::epic
