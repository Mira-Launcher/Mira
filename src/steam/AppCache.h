#pragma once

#include <filesystem>
#include <vector>

#include "steam/SteamWebApi.h"

namespace mira::steam {

// Owned games from Steam's local appcache files, for when there's no Web API key.
// Games in the packages the account owns, sorted by name; playtime is left 0.
std::vector<OwnedGame> OwnedFromAppCache(const std::filesystem::path& steam_root);

}  // namespace mira::steam
