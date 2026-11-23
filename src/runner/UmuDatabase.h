#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "model/Types.h"

namespace mira::config {
class Config;
}

namespace mira::runner {

// The GAMEID and STORE protonfixes look a game's fixes up by.
struct UmuMatch {
  std::string gameid;  // "umu-397540"
  std::string store;   // "egs"; empty when its store isn't known
  std::string title;   // the game's name in the umu database, when it's listed
};

// Downloads the umu database when the copy Mira keeps is over a week old. Blocks; best effort.
void RefreshUmuDatabase(const config::Config& config);

// A row of the umu database.
struct UmuRow {
  std::string umu_id;  // "umu-397540"
  std::string title;
};

// The row for a game's id in a store ("gog", "egs", ...); nullopt when it isn't listed, or
// umu.match is off.
std::optional<UmuRow> UmuRowFor(const config::Config& config, const std::string& store,
                                const std::string& code);

// The row with exactly this title, when only one game has it; nullopt otherwise.
std::optional<UmuRow> UmuRowByTitle(const config::Config& config, const std::string& title);

// The GAMEID and STORE a game runs with: what's set in its runner options wins, "none" there turns
// it off, then library::Identify's, then an id of its own.
UmuMatch UmuIdentity(const config::Config& config, const model::Game& game);

// The GAMEID and STORE to run on the Proton build at `build_dir`: a Steam game's bare appid when
// that's where the build files its fix, since protonfixes never looks there for "umu-<appid>".
// Running with a bare appid, umu sets SteamAppId to 0, so this is only done for a fix.
UmuMatch ForBuild(const std::filesystem::path& build_dir, UmuMatch identity);

// Whether the Proton build at `build_dir` ships protonfixes with a fix for this GAMEID and STORE.
bool HasProtonFix(const std::filesystem::path& build_dir, const UmuMatch& identity);

// The Steam appid in an umu GAMEID ("umu-397540" -> "397540"), or empty.
std::string SteamAppId(const std::string& gameid);

}  // namespace mira::runner
