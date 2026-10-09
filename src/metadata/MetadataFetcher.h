#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
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

// A store title not installed yet: its details (store info, ProtonDB tier) and a small cover, each
// only while missing. The store's own cover where there is one, else SteamGridDB's. Installing
// runs Fetch.
Result<void> FetchTitle(const config::Config& config, store::MetadataStore& cache, const model::Game& game);

// A tracked game's details (store info, reviews, ProtonDB tier) fetched again into its cached
// record, art untouched. For details older than metadata.refresh_days.
Result<void> RefreshDetails(const config::Config& config, store::MetadataStore& cache, const model::Game& game);

// FetchTitle for Steam titles (runner_ref "steam:<appid>"), up to about 50: one store request for
// all their info, reviews and cover addresses. Results line up with `titles`.
std::vector<Result<void>> FetchSteamTitles(const config::Config& config, store::MetadataStore& cache,
                                           const std::vector<model::Game>& titles);

// Whether a store title is missing its cover, or its details are missing or older than
// metadata.refresh_days.
bool TitleNeedsFetch(const config::Config& config, const store::MetadataStore& cache, const std::string& id);

// Whether `info`'s details were fetched within metadata.refresh_days.
bool DetailsFresh(const config::Config& config, const nlohmann::json& info);

// Shrinks art saved before downloads were fitted, and drops the unused capsule and header
// images, once per game. Stops between games when its Lane task is stopped. Temporary: drop
// it with cache.db's fit_pending table a release or two later.
void FitCachedArt(const config::Config& config, store::MetadataStore& cache,
                  const std::function<bool(const std::string&)>& is_title);

// Removes art folders with no cached record (a removed game's, a failed first fetch's), once
// they're a day old so a fetch still writing its first record is left alone.
void PruneOrphanArt(const config::Config& config, const store::MetadataStore& cache);

// SteamGridDB's matches for `name`, best first: [{id, name, release_date?}].
// Fetch uses the first unless the game sets metadata.steamgriddb_id.
Result<nlohmann::json> SearchSteamGridDb(const config::Config& config, const std::string& name);

// Re-downloads one SteamGridDB candidate (by the id it was listed with in
// info["art_candidates"][slot], from a prior Fetch()) and makes it the
// active image for `slot`, leaving every other cached slot untouched.
Result<void> SelectArtwork(const config::Config& config, store::MetadataStore& cache, const std::string& game_id,
                           const std::string& slot, std::int64_t candidate_id);

// Makes a PNG or JPEG the user supplied `slot`'s image, fitted like any art and kept, like a
// pick, through refreshes.
Result<void> UploadArtwork(const config::Config& config, store::MetadataStore& cache, const std::string& game_id,
                           const std::string& slot, std::string_view bytes);

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
// One game's, as its settings close.
void ClearCandidateThumbs(const config::Config& config, const std::string& game_id);

// Each cached game's Steam tags as last fetched with its metadata (most voted first), by id: empty
// when no Steam game matched, nullopt when fetched before Steam tags were. A game with no metadata
// record isn't listed. One query, so listing the whole library stays quick.
std::unordered_map<std::string, std::optional<std::vector<std::string>>> StoredSteamTags(
    const store::MetadataStore& cache);
// Whether `game` should have Steam tags: tags.steam, not a store's launcher, and a Steam game or
// metadata.steam_by_name.
bool WantsSteamTags(const config::Config& config, const model::Game& game);
// Fetches the Steam tags of those of `games` that want them and have a metadata record (a game
// without one gets them with its own fetch), batched, into those records.
// Returns how many records got an answer (a game with no Steam match counts, with no tags).
int FetchSteamTags(const config::Config& config, store::MetadataStore& cache, std::span<const model::Game> games);

// Where a game's art is downloaded: beside settings.toml, the same folder
// store::MetadataStore::ArtworkDir names.
std::filesystem::path ArtworkDir(const config::Config& config, const std::string& game_id);

// Whether Steam lists an app named exactly `name` as software (true) or a game (false); nullopt when no exact match.
std::optional<bool> SteamSaysSoftware(const std::string& name);

}  // namespace mira::metadata
