#pragma once

#include <json.hpp>
#include <string>

#include "model/Types.h"

namespace mira::config {
class Config;
}

namespace mira::library {

// Which game a library game is. The one place Mira decides it: art and store details, ProtonDB,
// saves, protonfixes, anti-cheat and the Proton picked all ask Identify.
struct GameIdentity {
  // Its store as umu and protonfixes name them ("steam", "gog", "egs", "amazon", "humble",
  // "itchio", "ubisoft", "ea", "battlenet") and its id there; empty when not known.
  std::string store;
  std::string store_id;
  std::string steam_appid;  // the Steam game it is; empty when not known
  std::string umu_id;       // its umu GAMEID ("umu-1151640"); empty when not known
  std::string title;        // its name in the umu database or GOG's file, when either names it
  // Where steam_appid came from: "override" (metadata.steam_appid), "steam" (a Steam game),
  // "file" (a steam_appid.txt beside it), "umu" (the umu database) or "name" (Steam's game of its
  // exact name, which a metadata fetch found); empty with no steam_appid.
  std::string steam_from;
};

// Its identity from, best first: the game's own metadata.steam_appid, the store it was imported
// from, what the store left in its folder (GOG's goggame-<id>.info, steam_appid.txt), the umu
// database (by store id, else exact title), then the Steam game of its exact name. Reads only local
// files; no network.
GameIdentity Identify(const config::Config& config, const model::Game& game);

nlohmann::json ToJson(const GameIdentity& identity);

}  // namespace mira::library
