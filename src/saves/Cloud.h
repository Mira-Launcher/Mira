#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <json.hpp>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"
#include "saves/Resolve.h"
#include "store/GameStore.h"
#include "store/MetadataStore.h"

// Syncing a GOG or Epic game's saves with its store's cloud. Mira decides the direction itself,
// from what it recorded at the last sync, and only then asks gogdl or legendary to move the files
// that way: neither tool's own decision is used, since gogdl reports a conflict as a sync and
// legendary has no conflicts at all.
namespace mira::saves {

enum class Direction { Download, Upload };

// One folder a game's cloud saves are kept in, as its store names and writes it.
struct CloudLocation {
  std::string name;     // GOG's location name ("__default" when it lists none); "saves" for Epic
  std::string pattern;  // in the store's placeholder form, read by the shared resolver
};

// What a store says about a game's cloud saves; cached in cache.db.
struct CloudInfo {
  bool supported = false;
  std::string reason{};  // why not, when not
  std::vector<CloudLocation> locations{};
  std::string client_id{};      // GOG's, for the cloud's token
  std::string client_secret{};  // likewise; never logged or answered
};
nlohmann::json CloudInfoJson(const CloudInfo& info);
CloudInfo CloudInfoFromJson(const nlohmann::json& value);

// What one cloud location holds now.
struct CloudListing {
  std::int64_t count = 0;
  std::int64_t newest_ns = 0;  // its newest file or upload, unix ns
  // Each file's md5 by its path in the location, where the store gives them (GOG).
  std::map<std::string, std::string> md5{};
};

// A store's half of syncing. GOG's and Epic's live in src/gog and src/epic.
class CloudSide {
 public:
  virtual ~CloudSide() = default;
  virtual std::string_view Name() const = 0;  // "GOG", "Epic Games"
  // Asks the store whether and where the game keeps cloud saves.
  virtual Result<CloudInfo> Info(const config::Config& config, const model::Game& game) const = 0;
  virtual Result<CloudListing> List(const config::Config& config, const model::Game& game,
                                    const CloudInfo& info, const CloudLocation& location) const = 0;
  // Replaces one side's files with the other's for `location`, kept locally in `folder`.
  virtual Result<void> Move(const config::Config& config, const model::Game& game,
                            const CloudInfo& info, const CloudLocation& location,
                            const std::filesystem::path& folder, Direction direction) const = 0;
};

// The cloud side of `game`'s store, or null when Mira syncs none for it.
const CloudSide* CloudFor(const model::Game& game);

// A location's state, and the whole game's (the most pressing of its locations').
inline constexpr std::string_view kSynced = "synced";
inline constexpr std::string_view kLocalNewer = "local_newer";
inline constexpr std::string_view kCloudNewer = "cloud_newer";
inline constexpr std::string_view kConflict = "conflict";
inline constexpr std::string_view kUnknown = "unknown";

// The local files of a cloud location, summed up so a later look can tell whether they changed:
// their paths, sizes and mtimes. "none" when there are none.
std::string LocalMarker(const std::filesystem::path& folder, const std::vector<SaveFile>& files);

// The literal folder a cloud location names for this game; empty when it can't be told (an
// unknown placeholder, a wildcard, or too broad a folder).
std::filesystem::path CloudFolder(const CloudLocation& location, const Roots& roots);

// Whether the store said lately that `game` keeps no cloud saves, so a sync has nothing to ask.
bool KnownUnsupported(const store::MetadataStore& cache, const model::Game& game);

// The patterns of the cloud locations cached for `game`, without asking the store: backups take
// these folders too.
std::vector<std::string> CachedCloudPatterns(const store::MetadataStore& cache,
                                             const model::Game& game);

// GET /v1/games/{id}/saves's `cloud`, from what's stored and the disk, without the network:
// {store, state, synced_at, checked_at, locations: [{name, path, state}], reason?}, or null for
// a game whose store has no cloud saves Mira syncs. `state` "off" while saves.cloud is off,
// "unsupported" with `reason`, else a location state.
nlohmann::json CloudOverview(const config::Config& config, const store::MetadataStore& cache,
                             const store::GameStore& games, const model::Game& game);

enum class CloudAction { Check, Sync, Download, Upload };

// Classifies each cloud location (asking the store), then for Sync moves each the way it's
// behind, for Download and Upload the way asked. `before_download` runs once before anything
// local is overwritten (a snapshot); its failure stops the sync. The new state is stored.
// {state, locations: [{name, path, state, moved?}]}. Errs saves_conflict (Sync, with nothing
// moved), cloud_unsupported, the store's errors.
Result<nlohmann::json> RunCloud(const config::Config& config, store::MetadataStore& cache,
                                store::GameStore& games, const model::Game& game,
                                CloudAction action,
                                const std::function<Result<void>()>& before_download);

}  // namespace mira::saves
