#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"
#include "store/MetadataStore.h"

namespace mira::metadata {

// Cover art + store metadata for a game, cached on disk under
// paths::UserDir() (never in games.toml, since it's always re-fetchable).
// Steam-owned games use Steam's store API + ProtonDB + Steam's CDN, all
// keyless; everything else uses SteamGridDB (needs steamgriddb.api_key, or
// fails with no_steamgriddb_key). Shells out to curl rather than linking
// libcurl, same as runner/Downloader.cpp. Synchronous; see
// metadata/FetchQueue.h for the background-safe wrapper.
Result<void> Fetch(const config::Config& config, store::MetadataStore& cache, const model::Game& game);

// Only a cover, for a store title not installed yet: the store's own where
// there is one (Steam's CDN, Epic via Legendary, GOG's gamesdb for GOG, itch
// and Amazon), else SteamGridDB's. Much cheaper than Fetch across a whole
// store library; installing runs Fetch.
Result<void> FetchCover(const config::Config& config, store::MetadataStore& cache, const model::Game& game);

// SteamGridDB's matches for `name`, best first: [{id, name, release_date?}].
// Fetch uses the first unless the game sets metadata.steamgriddb_id.
Result<nlohmann::json> SearchSteamGridDb(const config::Config& config, const std::string& name);

// Re-downloads one SteamGridDB candidate (by the id it was listed with in
// info["art_candidates"][slot], from a prior Fetch()) and makes it the
// active image for `slot`, leaving every other cached slot untouched.
Result<void> SelectArtwork(const config::Config& config, store::MetadataStore& cache, const std::string& game_id,
                           const std::string& slot, std::int64_t candidate_id);

// One page (50) of SteamGridDB's art for a game's slot, asked for now, so the
// current steamgriddb.nsfw applies and results past the first 50 a metadata
// fetch cached are reachable: {"page", "total", "candidates": [...]}, each
// candidate shaped like art_candidates' and added to that cached list.
Result<nlohmann::json> FetchCandidatePage(const config::Config& config, store::MetadataStore& cache,
                                          const std::string& game_id, const std::string& slot, int page);

// Which candidates of one FetchCandidateThumbs batch have a preview on disk.
struct ThumbBatch {
  std::vector<std::int64_t> ready;
  std::vector<std::int64_t> failed;
};

// Downloads the preview of each listed art_candidates[slot] entry not cached
// yet, all at once: SteamGridDB's small `thumb`, else the image itself.
// Looked up by id, like SelectArtwork, so no caller-supplied URL is fetched.
Result<ThumbBatch> FetchCandidateThumbs(const config::Config& config, const store::MetadataStore& cache,
                                        const std::string& game_id, const std::string& slot,
                                        const std::vector<std::int64_t>& candidate_ids);

// One candidate's cached preview, or an empty path if it isn't fetched yet.
std::filesystem::path CandidateThumbFile(const config::Config& config, const std::string& game_id,
                                         const std::string& slot, std::int64_t candidate_id);

// Previews are only for a picker that's open, so they're thrown away when
// the GUI quits and when mirad starts or stops, rather than kept like art.
void ClearCandidateThumbs(const config::Config& config);

// Where a game's art is downloaded: beside settings.toml, the same folder
// store::MetadataStore::ArtworkDir names.
std::filesystem::path ArtworkDir(const config::Config& config, const std::string& game_id);

}  // namespace mira::metadata
