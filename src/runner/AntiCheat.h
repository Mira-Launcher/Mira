#pragma once

#include <optional>
#include <string>
#include <vector>

#include "model/Types.h"

namespace mira::config {
class Config;
}

namespace mira::runner {

// A game's entry in AreWeAntiCheatYet: whether its anti-cheat lets it run on Linux.
struct AntiCheat {
  std::string status;                   // "Supported", "Running", "Planned", "Broken" or "Denied"
  std::vector<std::string> anticheats;  // "Easy Anti-Cheat"
  std::string url;                      // the page saying so, when there is one

  // Its anti-cheat stops it online, or outright.
  bool Blocks() const { return status == "Broken" || status == "Denied"; }
};

// Downloads the AreWeAntiCheatYet list when Mira's copy is over a week old. Blocks; best effort.
void RefreshAntiCheatDatabase(const config::Config& config);

// `game` in the list, by its Steam appid (a Steam game's, or its umu GAMEID's), else its exact
// title. nullopt when it isn't listed, or anticheat.check is off.
std::optional<AntiCheat> FindAntiCheat(const config::Config& config, const model::Game& game);

}  // namespace mira::runner
