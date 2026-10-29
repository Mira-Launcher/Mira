#pragma once

#include <QImage>
#include <QObject>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../Types.h"

// The whole library: listing, scanning, imports, moving games and menu entries.
// Each call runs on a worker thread and delivers its result on the main thread
// (client/Async.h), so the UI never blocks on the socket. See docs/api.md.
namespace mira_gui::api {

// GET /v1/games?include_hidden=true: the whole library, hidden games included.
void ListAllGamesAsync(QObject* context, std::function<void(GamesResult)> callback);

// POST /v1/library/scan. Runs synchronously on mirad's side, hence
// its own read timeout.
void ScanLibraryAsync(QObject* context, std::function<void(ScanResult)> callback);

// POST /v1/steam/scan. Idempotent: updates Steam-owned fields without
// touching anything the user configured.
void ScanSteamAsync(QObject* context, std::function<void(SteamScanResult)> callback);

// POST /v1/steam/status: `status` is "online" or "invisible". Fails while Steam isn't running.
void SetSteamStatusAsync(QObject* context, const std::string& status,
                         std::function<void(StoreActionResult)> callback);

// POST /v1/steam/shortcut: keeps Steam's "Mira" shortcut running `exe` with `launch_options`.
void UpdateSteamShortcutAsync(QObject* context, const std::string& exe,
                              const std::string& launch_options);

// POST /v1/steam/bigpicture.
void OpenSteamBigPictureAsync(QObject* context, std::function<void(StoreActionResult)> callback);

// GET /v1/lutris or /v1/heroic (`importer`): whether that launcher is on this computer.
void GetImporterAsync(QObject* context, const std::string& importer,
                      std::function<void(ImporterStatusResult)> callback);

// Upserts every wine game Lutris has, reading Lutris's own database and
// configs. Nothing on disk moves; see docs/api.md, POST /v1/lutris/import.
void ImportLutrisAsync(QObject* context, std::function<void(LutrisImportResult)> callback);

// Upserts the games Heroic installed, from its own files. Nothing on disk moves; see
// docs/api.md, POST /v1/heroic/import.
void ImportHeroicAsync(QObject* context, std::function<void(HeroicImportResult)> callback);

// GET /v1/desktop-entries/candidates: already-installed .desktop entries
// (including Flatpak apps, via their X-Flatpak key) that could become
// games. An empty list when desktop_import.enabled is off, not an error.
void GetDesktopEntryCandidatesAsync(QObject* context,
                                    std::function<void(DesktopEntryCandidatesResult)> callback);

// POST /v1/desktop-entries/import. `ids` are candidate ids as returned by
// GetDesktopEntryCandidatesAsync; those also in `apps` are tagged app.
void ImportDesktopEntriesAsync(QObject* context, const std::vector<std::string>& ids,
                               const std::vector<std::string>& apps,
                               std::function<void(DesktopEntryImportResult)> callback);

// POST /v1/desktop-entries/sync: regenerates Mira's own desktop entries
// immediately, for right after changing desktop_entries.* settings.
void SyncDesktopEntriesAsync(QObject* context,
                             std::function<void(DesktopEntrySyncResult)> callback);

void RelocateLibraryAsync(QObject* context, std::function<void(RelocateLibraryResult)> callback);

// POST /v1/library/relocate for just `ids`: moves their files and prefixes
// into Mira's own layout, one game at a time.
void RelocateGamesAsync(QObject* context, const std::vector<std::string>& ids,
                        std::function<void(RelocateLibraryResult)> callback);

// GET /v1/updates. Reaches each release source, cached by mirad.
void GetUpdatesAsync(QObject* context, std::function<void(UpdatesResult)> callback);

// GET /v1/library/unclear: folders that could be any of several games moved by hand.
void ListUnclearMovesAsync(QObject* context, std::function<void(UnclearMovesResult)> callback);

// POST /v1/library/unclear: `folder` is game `id`'s, or a new game when `id` is empty.
void SettleUnclearMoveAsync(QObject* context, const std::string& folder, const std::string& id,
                            std::function<void(SettleMoveResult)> callback);

// GET /v1/library/drops: things dropped into a folder that Mira couldn't sort.
void ListUnclearDropsAsync(QObject* context, std::function<void(UnclearDropsResult)> callback);

// POST /v1/library/drops: sorts the dropped `path` by the chosen `choice` id, as a job.
void SortDropAsync(QObject* context, const std::string& path, const std::string& choice,
                   std::function<void(ImportPathResult)> callback);

// What a dropped path is: kind "game", "app" or "unknown".
void ClassifyImportAsync(QObject* context, const std::string& path,
                         std::function<void(ImportGuessResult)> callback);

// Moves it into the Games or Applications folder as a job; the library picks it up from there.
void ImportPathAsync(QObject* context, const std::string& path, const std::string& kind,
                     std::function<void(ImportPathResult)> callback);

}  // namespace mira_gui::api
