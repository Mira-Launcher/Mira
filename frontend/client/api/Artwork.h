#pragma once

#include <QImage>
#include <QObject>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../Types.h"

// Cover art, metadata and the SteamGridDB art picker.
// Each call runs on a worker thread and delivers its result on the main thread
// (client/Async.h), so the UI never blocks on the socket. See docs/api.md.
namespace mira_gui::api {

// DELETE /v1/artwork/thumbs, as the GUI quits: the art picker's previews
// aren't worth keeping on disk. Blocking for the same reason as above.
void ClearArtThumbsBlocking();

// One named art slot: "cover", "hero", "logo", "icon". Which ones exist depends on the source; GetMetadataAsync's
// art_slots says which were cached. Decoded off the UI thread; a null
// image when missing or undecodable.
void GetArtworkImageAsync(QObject* context, const std::string& id, const std::string& slot,
                          std::function<void(QImage)> callback);

// GET /v1/games/{id}/metadata. A 404 is ordinary (nothing fetched yet, or
// fetched and nothing found) and comes back as missing, not as an error.
void GetMetadataAsync(QObject* context, const std::string& id,
                      std::function<void(GameMetadataResult)> callback);

// GET /v1/library/metadata?source=&ref=: a store title's cached details, shaped like a game's.
void GetTitleMetadataAsync(QObject* context, const std::string& source, const std::string& ref,
                           std::function<void(GameMetadataResult)> callback);

// DELETE /v1/games/{id}/artwork/thumbs, as a game's settings close.
void ClearGameArtThumbsAsync(QObject* context, const std::string& id);

// POST /v1/games/{id}/metadata/refresh. Returns 202 immediately; watch for
// game.metadata_ready/.metadata_failed. `announce` marks this as
// user-initiated so mirad reports the outcome as a `notification` event.
void RefreshMetadataAsync(QObject* context, const std::string& id, bool announce,
                          std::function<void(MetadataRefreshResult)> callback);

// POST /v1/games/metadata/refresh: the same, unannounced, for many games, as
// one job; `callback` runs once every fetch has ended.
void RefreshMetadataManyAsync(QObject* context, const std::vector<std::string>& ids,
                              std::function<void(MetadataBatchResult)> callback);

// POST /v1/games/{id}/artwork?type=. `candidate_id` must be one of the ids
// GetMetadataAsync's cover_candidates listed. mirad looks it up rather
// than accepting a URL. Returns 202; watch for
// game.artwork_selected/.artwork_select_failed.
void SelectArtworkAsync(QObject* context, const std::string& id, const std::string& slot,
                        std::int64_t candidate_id,
                        std::function<void(ArtworkSelectResult)> callback);

// POST /v1/games/{id}/artwork/candidates?type=&page=: one page of
// SteamGridDB's art, asked for now; game.artwork_candidates_ready follows,
// carrying `request` (an alphanumeric token) back.
void FetchArtCandidatesAsync(QObject* context, const std::string& id, const std::string& slot,
                             int page, const std::string& request,
                             std::function<void(GameActionResult)> callback);

// POST /v1/games/{id}/artwork/thumbs?type=: caches previews of up to 64
// candidates in the background; game.artwork_thumbs_ready follows.
void FetchArtThumbsAsync(QObject* context, const std::string& id, const std::string& slot,
                         const std::vector<std::int64_t>& candidate_ids,
                         std::function<void(GameActionResult)> callback);

// GET .../artwork/thumb for each id, in one round of requests, decoded off
// the UI thread. An id without a decodable preview is left out.
void GetArtThumbsAsync(QObject* context, const std::string& id, const std::string& slot,
                       const std::vector<std::int64_t>& candidate_ids,
                       std::function<void(std::vector<std::pair<std::int64_t, QImage>>)> callback);

// POST /v1/games/metadata/refresh-missing: every game without a cover, as one job.
void RefreshMissingArtworkAsync(QObject* context,
                                std::function<void(MetadataBatchResult)> callback);

// GET /v1/games/{id}/metadata/matches[?q=]: which SteamGridDB game the
// art could come from. Empty `query` searches the game's name.
void GetGriddbMatchesAsync(QObject* context, const std::string& id, const std::string& query,
                           std::function<void(GriddbMatchesResult)> callback);

// POST .../metadata/match: take art from this SteamGridDB game (0: the top
// match) and refetch; game.metadata_ready follows.
void SetGriddbMatchAsync(QObject* context, const std::string& id, std::int64_t griddb_id,
                         std::function<void(GameActionResult)> callback);

// The same two fetches on the calling thread, for a caller that decodes
// the image on its own worker thread too.
ArtworkResult GetArtworkBlocking(const std::string& id, const std::string& slot);

ArtworkResult GetTitleArtworkBlocking(const std::string& source, const std::string& ref);

}  // namespace mira_gui::api
