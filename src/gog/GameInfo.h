#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace mira::gog {

// The goggame-<id>.info GOG puts in a game's folder, from its offline installers and gogdl alike.
struct GameInfo {
  std::string id;    // the GOG product id, "1207658883"
  std::string name;  // empty when the file doesn't say
};

// The base game's file in `dir` (a DLC has its own, naming the base game as its rootGameId), else
// the first; nullopt when there is none.
std::optional<GameInfo> ReadGameInfo(const std::filesystem::path& dir);

}  // namespace mira::gog
