#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "model/Types.h"

namespace mira::config {
class Config;
}

namespace mira::runner {

// A game's entry in the umu database: the GAMEID and STORE protonfixes look its fixes up by.
struct UmuMatch {
  std::string gameid;  // "umu-397540"
  std::string store;   // "egs", or "none" for a match by title
  std::string title;   // the database's name for it
};

// Downloads the umu database when the copy Mira keeps is over a week old. Blocks; best effort.
void RefreshUmuDatabase(const config::Config& config);

// `game` in the umu database: by its store and id there, else by an exact title. nullopt when
// it's not listed, or umu.match is off.
std::optional<UmuMatch> FindUmu(const config::Config& config, const model::Game& game);

// The GAMEID and STORE a game runs with: what's set in its runner options wins, "none" there turns
// the lookup off, then the database's match, then an id of its own.
UmuMatch UmuIdentity(const config::Config& config, const model::Game& game);

// Whether the Proton build at `build_dir` ships protonfixes with a fix for this GAMEID and STORE.
bool HasProtonFix(const std::filesystem::path& build_dir, const UmuMatch& identity);

// The Steam appid in an umu GAMEID ("umu-397540" -> "397540"), or empty.
std::string SteamAppId(const std::string& gameid);

}  // namespace mira::runner
