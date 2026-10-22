#pragma once

#include <optional>
#include <string>

#include "config/Config.h"
#include "model/Types.h"
#include "store/MetadataStore.h"

namespace mira::saves {

struct Match {
  store::MetadataStore::SaveEntry entry;  // aliases followed: the entry with the save locations
  // How it was found: "setting" (saves.manifest_name), "steam", "gog", "lutris" or "name".
  std::string rule;
};

// `game`'s manifest entry, by the first rule that finds one: the game's saves.manifest_name, its
// Steam app id (a Steam game's, or metadata.steam_appid), its GOG product id, its Lutris slug (as
// an id, then read as a name), then its exact Normalize'd name. Never a guess by similarity: a
// wrong entry would back up the wrong files.
std::optional<Match> FindEntry(const config::Config& config, const store::MetadataStore& cache,
                               const model::Game& game);

}  // namespace mira::saves
