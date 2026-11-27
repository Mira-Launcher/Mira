#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "config/Config.h"
#include "store/GameStore.h"

namespace mira::epic {

// The playtime Epic recorded for each of the account's games, in seconds by legendary app name,
// from Epic's library service with legendary's session (renewed through legendary first when it
// expired). A game never played through Epic isn't listed. Nothing when signed out or unreachable.
std::optional<std::map<std::string, std::int64_t>> Playtime(const config::Config& config);

// Adds to a game's playtime what Epic counted since Mira last took its total, so time from the
// Epic Games Launcher and other computers counts once and what Mira sent isn't counted back. The
// first time, Mira keeps the higher of the two.
void TakePlaytime(store::GameStore& games, const std::string& id, std::int64_t epic_total);

// Sends Epic the game's sessions that ended since its last send, as the Epic Games Launcher reports
// play. Nothing before the game's first TakePlaytime is sent. A failed send is tried again next
// time.
void SendPlaytime(const config::Config& config, store::GameStore& games, const std::string& id);

}  // namespace mira::epic
