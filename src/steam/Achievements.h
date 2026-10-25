#pragma once

#include <string>

#include "achievements/Achievements.h"
#include "config/Config.h"
#include "core/Result.h"

namespace mira::steam {

// A Steam game's achievements: every definition from appcache/stats/UserGameStatsSchema_<appid>.bin
// and the account's progress from UserGameStats_<account>_<appid>.bin, which Steam writes for games
// it has run on this computer. Without them, Steam's Web API when steam.web_api_key and
// steam.steamid64 are set. Rarity from Steam's keyless global percentages, when it answers.
Result<achievements::GameAchievements> FetchAchievements(const config::Config& config,
                                                         const std::string& appid,
                                                         const achievements::Locale& locale);

}  // namespace mira::steam
