#pragma once

#include <QImage>
#include <QObject>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Types.h"

// Parsers for the `data:` JSON of the events client/EventStream delivers.
namespace mira_gui::events {

bool ParseGameSummary(const std::string& data, GameSummary* out);

// A games.updated payload's `games`.
bool ParseGameSummaries(const std::string& data, std::vector<GameSummary>* out);

// Parses a `game.state` payload (`{"id", "state": "running" | "exited" |
// "crashed", ...}`, docs/api.md) down to just id/state, enough to know
// which row's Launch/Stop button to flip. Unlike game.added/updated this
// carries no other game fields (not even play_seconds), so an
// "exited"/"crashed" state is a signal to re-fetch, not something to patch
// a row from directly.
bool ParseGameState(const std::string& data, GameStateEvent* out);

// Parses `game.launched`. `tracked` defaults to false when the payload
// omits it: an older daemon published this event only for the case where
// nothing was watching, so that is what its silence meant.
bool ParseGameLaunched(const std::string& data, GameLaunchedEvent* out);

bool ParseInstallDetected(const std::string& data, InstallDetectedEvent* out);

// Parses `library.move_unclear`'s payload ({folder, games: [{id, name}]}); false without a folder
// or with fewer than two games.
bool ParseUnclearMove(const std::string& data, UnclearMove* out);
// The folder of `library.move_settled` ({folder}), or empty.
std::string ParseSettledFolder(const std::string& data);

// Parses `game.removed`'s payload (`{"id": "..."}`, Server.cpp).
std::string ParseRemovedId(const std::string& data);

// Parses `games.removed`'s payload (`{"ids": [...]}`).
std::vector<std::string> ParseRemovedIds(const std::string& data);

// Parses a `game.metadata_ready`/`.metadata_failed` payload. `state` is
// the event type, which the payload does not repeat; false if `data` is
// not a JSON object with an id.
bool ParseMetadataEvent(const std::string& data, MetadataEvent* out);

// Parses a `game.artwork_selected`/`.artwork_select_failed` payload.
bool ParseArtworkSelectEvent(const std::string& data, ArtworkSelectEvent* out);

// Parses a `game.artwork_candidates_ready` payload.
bool ParseArtCandidatesEvent(const std::string& data, ArtCandidatesEvent* out);

// Parses a `game.artwork_thumbs_ready` payload.
bool ParseArtThumbsEvent(const std::string& data, ArtThumbsEvent* out);

// Parses a `notification` payload (`{"level": "...", "message": "..."}`).
// False if `data` is not a JSON object with a message.
bool ParseNotification(const std::string& data, NotificationEvent* out);

// library.artwork_ready/_failed: kind "artwork", state "ready"/"failed",
// error the failure's code.
bool ParseTitleArtworkEvent(const std::string& event_type, const std::string& data,
                            StoreEvent* out);

// A game.added payload's `open_config` (open_config_on_add).
bool ParseOpenConfig(const std::string& data);

// game.added's `auto_install`: the scan runs this installer on its own.
bool ParseAutoInstall(const std::string& data);

bool ParseInstallerLeftover(const std::string& data, InstallerLeftoverEvent* out);

bool ParseInstallEvent(const std::string& event_type, const std::string& data, InstallEvent* out);

bool ParseStoreEvent(const std::string& event_type, const std::string& data, StoreEvent* out);

// Parses a `runners.download.started`/`.finished`/`.failed` payload.
// `state` comes from the event type, which the payload itself doesn't
// repeat.
bool ParseRunnerDownload(const std::string& event_type, const std::string& data,
                         RunnerDownloadEvent* out);

// Parses a `tricks.started`/`.finished`/`.failed` payload. `state` comes
// from the event type, prefix-matched the same way ParseRunnerDownload is.
bool ParseTricksEvent(const std::string& event_type, const std::string& data, TricksEvent* out);

}  // namespace mira_gui::events
