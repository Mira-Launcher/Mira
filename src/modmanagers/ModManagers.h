#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"
#include "store/GameStore.h"

// Windows mod managers that cover many games (Vortex, Mod Organizer 2). Each is a source:
// downloaded once, then added to Windows games as a tool launch option that runs it in that game's
// own prefix, where it sees the game the way the game sees itself.
namespace mira::modmanagers {

struct ModManager {
  std::string id;  // also its source and its launch option's id
  std::string name;
  std::string release_url;                // a GitHub releases API url
  std::string asset;                      // the release asset, a glob
  std::vector<std::string> exclude = {};  // asset names with these in them are passed over
  std::string inner = {};  // an archive inside the download that holds it (Vortex's installer)
  std::string binary;      // its program, inside its folder
  std::vector<std::string> tricks =
      {};  // winetricks verbs a game's prefix gets before it first runs there
};

const std::vector<ModManager>& All();
const ModManager* Find(std::string_view id);

// Where Mira keeps it; its program is Dir / binary.
std::filesystem::path Dir(const ModManager& manager);
bool Installed(const ModManager& manager);
// The release tag of the copy installed, empty for none.
std::string InstalledVersion(const ModManager& manager);
// Downloads the latest release over the copy installed.
Result<void> Install(const ModManager& manager);
// Deletes Mira's copy and takes it off every game it's still on. Removing its source purges each
// game first (PurgeFromGame).
Result<void> Uninstall(store::GameStore& games, const ModManager& manager);

// The Windows games it can be added to: those with a prefix.
bool CanAdd(const model::Game& game);
bool OnGame(const model::Game& game, const ModManager& manager);
// mod_manager_open: why it can't be removed from `game_name` while open there.
Error OpenError(const ModManager& manager, std::string_view game_name);
// Readies the game's prefix with its tricks, then adds it to the game as a tool launch option.
Result<model::Game> AddToGame(config::Config& config, store::GameStore& games,
                              const ModManager& manager, const std::string& game_id);
Result<model::Game> RemoveFromGame(store::GameStore& games, const ModManager& manager,
                                   const std::string& game_id);

}  // namespace mira::modmanagers
