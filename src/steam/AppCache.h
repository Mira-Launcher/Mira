#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "steam/SteamWebApi.h"

namespace mira::steam {

// Owned games from Steam's local appcache files, for when there's no Web API key.
// Games in the packages the account owns, in Steam's library order; playtime is left 0.
std::vector<OwnedGame> OwnedFromAppCache(const std::filesystem::path& steam_root);

// Steam's library order: by each game's `sortas` name in the app cache where it has one ("Binding
// of Isaac: Rebirth"), else by its name, ignoring case.
void SortLikeSteam(std::vector<OwnedGame>& games,
                   const std::optional<std::filesystem::path>& steam_root);

struct AppSize {
  std::int64_t download_bytes = -1;  // -1: the cache doesn't say
  std::int64_t install_bytes = -1;
};

// What installing `appid` downloads and takes on disk, summed from the public manifests of the
// depots Steam would install: the Linux ones when the game has any, else Windows, the 64-bit build
// over the 32-bit one, with the DLC the account owns, without shared redistributables or other
// languages. nullopt when the app cache doesn't list its depots.
std::optional<AppSize> AppSizeFromAppCache(const std::filesystem::path& steam_root,
                                           const std::string& appid);

// The folder of the Proton Valve's Steam Play config picks for `appid` (app 891390's
// app_mappings), when that Proton is installed in one of Steam's libraries.
std::optional<std::filesystem::path> ValveProtonFor(const std::filesystem::path& steam_root,
                                                    const std::string& appid);

}  // namespace mira::steam
