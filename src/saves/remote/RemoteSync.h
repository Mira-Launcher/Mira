#pragma once

#include <functional>
#include <json.hpp>
#include <string>

#include "config/Config.h"
#include "model/Types.h"
#include "saves/Cloud.h"
#include "store/GameStore.h"
#include "store/MetadataStore.h"

// Syncing one game's saves with the user's own remote.
namespace mira::saves::remote {

struct Device {
  std::string id;
  std::string name;
};

// The folder a game's saves have on every remote: its store's id, its save list entry's name, else
// its own name ("name-...", which may be another game's). Empty when it can't be told.
std::string KeyFor(const model::Game& game, const std::string& entry_name);

// Compares the game's saves with the primary remote's, then for Sync moves them the way they're
// behind, for Download and Upload the way asked. `before_download` runs once before anything local
// is replaced. The state is stored. {state, revision, moved?, other: {device, at, files, bytes}?}.
// Errs remote_none, saves_conflict (Sync, with nothing moved), saves_too_large, Apply's and
// rclone's errors.
Result<nlohmann::json> RunRemote(const config::Config& config, const store::MetadataStore& cache,
                                 store::GameStore& games, const model::Game& game,
                                 const Device& device, CloudAction action,
                                 const std::function<Result<void>()>& before_download);

}  // namespace mira::saves::remote
