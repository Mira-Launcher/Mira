#pragma once

#include <string>

#include "achievements/Achievements.h"
#include "config/Config.h"
#include "core/Result.h"

namespace mira::gog {

// The signed-in account's id, from gogdl's token file; empty when signed out.
std::string UserId(const config::Config& config);

// A GOG game's achievements and the account's unlocks, from GOG Galaxy's gameplay API with the
// account's token. `product_id` is the game's GOG id (its source_ref). Undocumented: fails soft.
Result<achievements::GameAchievements> FetchAchievements(const config::Config& config,
                                                         const std::string& product_id,
                                                         const achievements::Locale& locale);

}  // namespace mira::gog
