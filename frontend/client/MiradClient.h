#pragma once

#include <QImage>
#include <QObject>

#include <functional>
#include <string>
#include <vector>

#include "Types.h"

// A minimal REST client for mirad, following the same contract as `mira`
// (src/cli/main.cpp): plain HTTP over the daemon's Unix domain socket, no
// dependency on mira_core. See docs/api.md and docs/architecture.md.
//
// Only endpoints live here. Transport.h owns the socket, the timeouts and
// the error envelope. JsonMapping.h owns the JSON conversions, and Async.h
// owns the thread hop. Methods run on a throwaway worker thread and deliver
// results back on the main thread, so UI never blocks on the socket.
namespace mira_gui {

class MiradClient {
public:
  // The socket every call above goes to. Exposed because the UI shows it.
  static std::string ResolveSocketPath();

  // GET /v1/health.
  static void CheckHealthAsync(QObject* context, std::function<void(HealthStatus)> callback);

  // GET /v1/games?include_hidden=true: the whole library, hidden games included.
  static void ListAllGamesAsync(QObject* context, std::function<void(GamesResult)> callback);

  // DELETE /v1/games/{id}[?delete_files=true][?delete_prefix=true][?delete_metadata=true].
  // All opt-in; false/omitted never touches disk. delete_metadata alone has
  // no library/prefix-root restriction: it's keyed by id under Mira's own state dir.
  static void DeleteGameAsync(QObject* context, const std::string& id, bool delete_files,
                              bool delete_prefix, bool delete_metadata,
                              std::function<void(DeleteResult)> callback);
  // POST /v1/games/delete: the same for many games in one request.
  static void DeleteGamesAsync(QObject* context, const std::vector<std::string>& ids, bool delete_files,
                               bool delete_prefix, bool delete_metadata,
                               std::function<void(DeleteGamesResult)> callback);

  // POST /v1/games/manual: adds a game record directly, for an installer or
  // a folder outside every library root. Response mirrors GetGameAsync.
  static void AddManualGameAsync(QObject* context, const std::string& install_path,
                                 const std::string& exe_path, const std::string& name,
                                 const std::string& platform, bool is_installer,
                                 std::function<void(GameDetailResult)> callback);

  // POST /v1/games/{id}/launch. Returns once the process exists, not once
  // it exits. 409 if the game isn't `ready` (message explains why, e.g.
  // needs_install) or 400 if the runner reference doesn't resolve.
  static void LaunchGameAsync(QObject* context, const std::string& id,
                              std::function<void(LaunchResult)> callback);

  // POST /v1/games/{id}/stop. SIGTERMs the game's process group; 409 if it
  // isn't currently running.
  static void StopGameAsync(QObject* context, const std::string& id,
                            std::function<void(StopResult)> callback);

  // POST /v1/library/scan. Runs synchronously on mirad's side, hence
  // its own read timeout.
  static void ScanLibraryAsync(QObject* context, std::function<void(ScanResult)> callback);

  // GET /v1/games/{id}, for the detail/edit view.
  static void GetGameAsync(QObject* context, const std::string& id,
                           std::function<void(GameDetailResult)> callback);

  // PATCH /v1/games/{id}. Setting any of these fields marks the game as reviewed.
  static void PatchGameAsync(QObject* context, const std::string& id, const GamePatch& patch,
                             std::function<void(PatchGameResult)> callback);

  // GET /v1/config/schema.
  static void GetConfigSchemaAsync(QObject* context,
                                   std::function<void(ConfigSchemaResult)> callback);

  // GET /v1/config, flattened.
  static void GetConfigAsync(QObject* context, std::function<void(ConfigResult)> callback);

  // PATCH /v1/config. A bad value anywhere in the patch means nothing
  // in it is applied.
  static void PatchConfigAsync(QObject* context, const std::vector<ConfigEdit>& edits,
                               std::function<void(PatchConfigResult)> callback);

  // POST /v1/config/reset?key=<dotted.key>.
  static void ResetConfigKeyAsync(QObject* context, const std::string& key,
                                  std::function<void(PatchConfigResult)> callback);

  // GET /v1/config, reading only the opaque `frontend` table.
  static void GetFrontendPrefsAsync(QObject* context,
                                    std::function<void(FrontendPrefsResult)> callback);

  // PATCH /v1/config with a `frontend` key. Merge-patch, so only the fields
  // set in `prefs` are written and a key this build doesn't know about
  // (an older or newer frontend's) survives untouched.
  static void SaveFrontendPrefsAsync(QObject* context, const FrontendPrefs& prefs,
                                     std::function<void(PatchConfigResult)> callback);

  // The same PATCH, run on the calling thread.
  //
  // For the one caller that has nowhere to deliver a result to and no time
  // to wait for one: a window saving its layout from closeEvent. The async
  // form hands the request to a detached thread, which the process can
  // outrun on its way out. One round trip over a Unix socket is cheap
  // enough to just wait for, and the short timeout below means an
  // unreachable daemon cannot turn quitting into a hang.
  static PatchConfigResult SaveFrontendPrefsBlocking(const FrontendPrefs& prefs);
  // DELETE /v1/artwork/thumbs, as the GUI quits: the art picker's previews
  // aren't worth keeping on disk. Blocking for the same reason as above.
  static void ClearArtThumbsBlocking();
  // For the window's size before it's first shown, so the compositor places
  // it at its real size.
  static FrontendPrefsResult GetFrontendPrefsBlocking();


  // One named art slot: "cover", "hero", "capsule", "header", "logo",
  // "icon". Which ones exist depends on the source; GetMetadataAsync's
  // art_slots says which were cached. Decoded off the UI thread; a null
  // image when missing or undecodable.
  static void GetArtworkImageAsync(QObject* context, const std::string& id, const std::string& slot,
                                   std::function<void(QImage)> callback);
  // The same two fetches on the calling thread, for a caller that decodes
  // the image on its own worker thread too.
  static ArtworkResult GetArtworkBlocking(const std::string& id, const std::string& slot);
  static ArtworkResult GetTitleArtworkBlocking(const std::string& source, const std::string& ref);

  // GET /v1/games/{id}/metadata/matches[?q=]: which SteamGridDB game the
  // art could come from. Empty `query` searches the game's name.
  static void GetGriddbMatchesAsync(QObject* context, const std::string& id, const std::string& query,
                                    std::function<void(GriddbMatchesResult)> callback);
  // POST .../metadata/match: take art from this SteamGridDB game (0: the top
  // match) and refetch; game.metadata_ready follows.
  static void SetGriddbMatchAsync(QObject* context, const std::string& id, std::int64_t griddb_id,
                                  std::function<void(GameActionResult)> callback);

  // GET /v1/games/{id}/metadata. A 404 is ordinary (nothing fetched yet, or
  // fetched and nothing found) and comes back as missing, not as an error.
  static void GetMetadataAsync(QObject* context, const std::string& id,
                               std::function<void(GameMetadataResult)> callback);

  // POST /v1/games/{id}/metadata/refresh. Returns 202 immediately; watch for
  // game.metadata_ready/.metadata_failed. `announce` marks this as
  // user-initiated so mirad reports the outcome as a `notification` event.
  static void RefreshMetadataAsync(QObject* context, const std::string& id, bool announce,
                                   std::function<void(MetadataRefreshResult)> callback);
  // POST /v1/games/metadata/refresh: the same, unannounced, for many games, as
  // one job; `callback` runs once every fetch has ended.
  static void RefreshMetadataManyAsync(QObject* context, const std::vector<std::string>& ids,
                                       std::function<void(MetadataBatchResult)> callback);

  // POST /v1/games/{id}/artwork?type=. `candidate_id` must be one of the ids
  // GetMetadataAsync's cover_candidates listed. mirad looks it up rather
  // than accepting a URL. Returns 202; watch for
  // game.artwork_selected/.artwork_select_failed.
  static void SelectArtworkAsync(QObject* context, const std::string& id, const std::string& slot,
                                 std::int64_t candidate_id,
                                 std::function<void(ArtworkSelectResult)> callback);

  // POST /v1/games/{id}/artwork/candidates?type=&page=: one page of
  // SteamGridDB's art, asked for now; game.artwork_candidates_ready follows,
  // carrying `request` (an alphanumeric token) back.
  static void FetchArtCandidatesAsync(QObject* context, const std::string& id, const std::string& slot, int page,
                                      const std::string& request, std::function<void(GameActionResult)> callback);
  // POST /v1/games/{id}/artwork/thumbs?type=: caches previews of up to 64
  // candidates in the background; game.artwork_thumbs_ready follows.
  static void FetchArtThumbsAsync(QObject* context, const std::string& id, const std::string& slot,
                                  const std::vector<std::int64_t>& candidate_ids,
                                  std::function<void(GameActionResult)> callback);
  // GET .../artwork/thumb for each id, in one round of requests, decoded off
  // the UI thread. An id without a decodable preview is left out.
  static void GetArtThumbsAsync(QObject* context, const std::string& id, const std::string& slot,
                                const std::vector<std::int64_t>& candidate_ids,
                                std::function<void(std::vector<std::pair<std::int64_t, QImage>>)> callback);

  // POST /v1/games/metadata/refresh-missing: every game without a cover, as one job.
  static void RefreshMissingArtworkAsync(QObject* context, std::function<void(MetadataBatchResult)> callback);

  // GET /v1/runners.
  static void ListRunnersAsync(QObject* context, std::function<void(RunnersResult)> callback);

  // GET /v1/runners/catalog?kind=proton|wine&source=: what is available
  // to install from one source (empty: the kind's preferred one). Unlike
  // everything else here this goes out to the GitHub API, so it has real
  // network latency and its own longer timeout.
  static void GetRunnerCatalogAsync(QObject* context, const std::string& kind, const std::string& source,
                                    std::function<void(RunnerCatalogResult)> callback);

  // GET /v1/runners/sources?kind=.
  static void ListRunnerSourcesAsync(QObject* context, const std::string& kind,
                                     std::function<void(RunnerSourcesResult)> callback);

  // GET /v1/runners/updates. Also reaches GitHub, cached by mirad.
  static void GetRunnerUpdatesAsync(QObject* context, std::function<void(RunnerUpdatesResult)> callback);

  // POST /v1/runners/update. Starts like a download; games on the old build
  // move to the new one when it finishes.
  static void UpdateRunnerAsync(QObject* context, const std::string& reference,
                                std::function<void(RunnerDownloadResult)> callback);

  // GET /v1/runners/tools and POST /v1/runners/tools/{id}/setup (reported as
  // <id>.setup.* events).
  static void ListRunnerToolsAsync(QObject* context, std::function<void(RunnerToolsResult)> callback);
  // GET /v1/runners/{kind}/schema: the options a kind's runner_config takes.
  static void GetRunnerSchemaAsync(QObject* context, const std::string& kind,
                                   std::function<void(RunnerSchemaResult)> callback);
  static void SetupRunnerToolAsync(QObject* context, const std::string& id,
                                   std::function<void(RunnerDownloadResult)> callback);

  // POST /v1/runners/download. Returns 202 as soon as the download starts;
  // the outcome arrives as a runners.download.finished/.failed event, since
  // a build can be 500+ MB.
  static void DownloadRunnerAsync(QObject* context, const std::string& kind,
                                  const std::string& tag, const std::string& source,
                                  std::function<void(RunnerDownloadResult)> callback);

  // POST /v1/steam/scan. Idempotent: updates Steam-owned fields without
  // touching anything the user configured.
  static void ScanSteamAsync(QObject* context, std::function<void(SteamScanResult)> callback);

  // Upserts every wine game Lutris has, reading Lutris's own database and
  // configs. Nothing on disk moves; see docs/api.md, POST /v1/lutris/import.
  static void ImportLutrisAsync(QObject* context,
                                std::function<void(LutrisImportResult)> callback);

  // POST /v1/games/{id}/run. Runs `exe_path` inside this game's prefix,
  // provisioning one on demand, which is how a needs_install game's
  // installer actually gets run, since Scanner never auto-provisions one.
  static void RunInPrefixAsync(QObject* context, const std::string& id,
                               const std::string& exe_path, const std::string& args,
                               std::function<void(RunInPrefixResult)> callback);

  // POST /v1/games/{id}/finish-install: flips a needs_install game to
  // ready once exe_path points at whatever the installer produced. 409 if
  // exe_path is still empty.
  // With `install_path` and `exe_path`, first switches the game to that
  // program installed in its prefix (game.install_detected).
  static void FinishInstallAsync(QObject* context, const std::string& id,
                                 std::function<void(FinishInstallResult)> callback,
                                 const std::string& install_path = std::string(),
                                 const std::string& exe_path = std::string());

  // GET /v1/games/{id}/config: this game's resolved settings, tagged by
  // layer (see GameConfigEntry).
  static void GetGameConfigAsync(QObject* context, const std::string& id,
                                 std::function<void(GameConfigResult)> callback);

  // PATCH /v1/games/{id}/config. Same all-or-nothing validation as
  // PATCH /v1/config (docs/api.md): only send edits that actually changed.
  static void PatchGameConfigAsync(QObject* context, const std::string& id,
                                   const std::vector<GameConfigEdit>& edits,
                                   std::function<void(PatchGameConfigResult)> callback);

  // PATCH /v1/games: tags and overrides for many games in one request.
  static void PatchGamesAsync(QObject* context, const GamesPatch& patch,
                              std::function<void(PatchGamesResult)> callback);

  // GET /v1/games/{id}/log?lines=. `lines` is how many trailing lines to ask
  // for; an empty result means nothing has ever been logged, not a failure.
  static void GetGameLogAsync(QObject* context, const std::string& id, int lines,
                              std::function<void(GameLogResult)> callback);

  // GET /v1/gamemode/status: whether the Feral GameMode daemon is installed
  // and reachable. Purely a status check; see GameModeStatusResult.
  static void GetGameModeStatusAsync(QObject* context,
                                     std::function<void(GameModeStatusResult)> callback);

  // POST /v1/games/{id}/tricks. Returns 202; watch for
  // tricks.started/.finished/.failed via ParseTricksEvent.
  static void RunWinetricksAsync(QObject* context, const std::string& id, const std::string& verb,
                                 std::function<void(TricksResult)> callback);

  // DELETE /v1/runners/{kind}:{name}. Synchronous: 200 once the build's
  // files are actually gone.
  static void DeleteRunnerAsync(QObject* context, const std::string& kind, const std::string& name,
                                std::function<void(RunnerRemoveResult)> callback);

  // GET /v1/desktop-entries/candidates: already-installed .desktop entries
  // (including Flatpak apps, via their X-Flatpak key) that could become
  // games. An empty list when desktop_import.enabled is off, not an error.
  static void GetDesktopEntryCandidatesAsync(
      QObject* context, std::function<void(DesktopEntryCandidatesResult)> callback);

  // POST /v1/desktop-entries/import. `ids` are candidate ids as returned by
  // GetDesktopEntryCandidatesAsync.
  static void ImportDesktopEntriesAsync(QObject* context, const std::vector<std::string>& ids,
                                        std::function<void(DesktopEntryImportResult)> callback);

  // POST /v1/desktop-entries/sync: regenerates Mira's own desktop entries
  // immediately, for right after changing desktop_entries.* settings.
  static void SyncDesktopEntriesAsync(QObject* context,
                                      std::function<void(DesktopEntrySyncResult)> callback);

  // --- SSE payload parsing -------------------------------------------------
  //
  // These take a `data:` line's raw JSON rather than making a request, since
  // the events arrive through EventStream, not through a call.

  // Parses a `game.added`/`game.updated` payload (Server.cpp publishes the
  // full model::ToJson(game) record for both) into the same summary
  // GET /v1/games returns. False unless `data` is a JSON object carrying a
  // non-empty string id: callers dispatch on the event type first, and this
  // is the second line of defence behind that.
  // --- Installers and relocation ---------------------------------------------

  // The game's own installer, or `path` (absolute, or relative to its
  // install folder) when choosing a different one.
  static void GetInstallerInfoAsync(QObject* context, const std::string& id, const std::string& path,
                                    std::function<void(InstallerInfoResult)> callback);
  // Detached; an InstallEvent follows. Empty `installer` uses the game's own.
  static void InstallGameAsync(QObject* context, const std::string& id, bool interactive,
                               const std::string& installer,
                               std::function<void(GameActionResult)> callback);
  static void GetInstallProgressAsync(QObject* context, const std::string& id,
                                      std::function<void(InstallProgressResult)> callback);
  static void RelocateLibraryAsync(QObject* context,
                                   std::function<void(RelocateLibraryResult)> callback);
  // POST /v1/library/relocate for just `ids`: moves their files and prefixes
  // into Mira's own layout, one game at a time.
  static void RelocateGamesAsync(QObject* context, const std::vector<std::string>& ids,
                                 std::function<void(RelocateLibraryResult)> callback);
  // POST /v1/games/{id}/relocate: moves one game's files to `install_path` and its prefix to
  // `data_dir`. An empty one stays where it is.
  static void RelocateGameAsync(QObject* context, const std::string& id, const std::string& install_path,
                                const std::string& data_dir, std::function<void(GameDetailResult)> callback);
  static bool ParseInstallEvent(const std::string& event_type, const std::string& data,
                                InstallEvent* out);

  // --- Stores (Epic, GOG, itch.io, Humble Bundle) ---------------------------

  static void GetStoreStatusAsync(QObject* context, const std::string& source,
                                  std::function<void(StoreStatusResult)> callback);
  // Downloads the store's helper tool as a job: the callback runs when it has finished or failed.
  static void SetupStoreToolAsync(QObject* context, const std::string& source,
                                  std::function<void(StoreActionResult)> callback);
  // `credential` is whatever the user pasted: a code, a whole login page or
  // redirect URL, an API key, or a session cookie, per store.
  static void SignInStoreAsync(QObject* context, const std::string& source,
                               const std::string& credential,
                               std::function<void(StoreActionResult)> callback);
  static void SignOutStoreAsync(QObject* context, const std::string& source,
                                std::function<void(StoreActionResult)> callback);
  static void ImportStoreAsync(QObject* context, const std::string& source,
                               std::function<void(StoreImportResult)> callback);
  // GET /v1/library?source=: owned titles, installed or not.
  static void GetStoreLibraryAsync(QObject* context, const std::string& source,
                                   std::function<void(StoreLibraryResult)> callback);
  static void InstallStoreTitleAsync(QObject* context, const std::string& source,
                                     const std::string& ref, bool update,
                                     std::function<void(StoreActionResult)> callback);
  // POST /v1/library/artwork: fetch covers for these titles, one at a time.
  // Each one ends in a library.artwork_ready/_failed event.
  static void QueueTitleArtworkAsync(QObject* context, const std::string& source,
                                     std::vector<StoreTitle> titles,
                                     std::function<void(StoreActionResult)> callback);
  // GET /v1/sources/{id}/removal and POST /v1/sources/{id}/remove.
  static void GetRemovalPlanAsync(QObject* context, const std::string& source,
                                  std::function<void(RemovalPlanResult)> callback);
  static void RemoveSourceAsync(QObject* context, const std::string& source,
                                std::function<void(RemoveSourceResult)> callback);
  // GET/POST /v1/sources/{id}/runner: a store's default runner, or a launcher's prefix runner.
  static void GetSourceRunnerAsync(QObject* context, const std::string& source,
                                   std::function<void(SourceRunnerResult)> callback);
  static void SetSourceRunnerAsync(QObject* context, const std::string& source, const std::string& runner_ref,
                                   bool apply_to_games, std::function<void(SourceRunnerResult)> callback);
  // GET/POST /v1/stores/itch/collections, DELETE /v1/stores/itch/collections/{id}.
  static void GetItchCollectionsAsync(QObject* context, std::function<void(ItchCollectionsResult)> callback);
  static void AddItchCollectionAsync(QObject* context, const std::string& link,
                                     std::function<void(StoreActionResult)> callback);
  static void RemoveItchCollectionAsync(QObject* context, std::int64_t id,
                                        std::function<void(StoreActionResult)> callback);
  static void GetHumbleLibraryAsync(QObject* context,
                                    std::function<void(HumbleLibraryResult)> callback);
  // A job: the callback runs when the download has finished or failed (`nothing_to_download` for a key-only bundle).
  static void DownloadHumbleBundleAsync(QObject* context, const std::string& bundle_key,
                                        std::function<void(HumbleDownloadResult)> callback);

  // The page to sign in at. Amazon's is made per attempt (PKCE), not a fixed page.
  static void BeginStoreLoginAsync(QObject* context, const std::string& source,
                                   std::function<void(LoginUrlResult)> callback);

  // --- Store launchers (Battle.net, Ubisoft Connect, EA app) ----------------

  static void GetLaunchersAsync(QObject* context, std::function<void(LaunchersResult)> callback);
  // Detached; a StoreEvent with kind "setup" and the launcher's id follows.
  static void InstallLauncherAsync(QObject* context, const std::string& id,
                                   std::function<void(StoreActionResult)> callback);
  static void ImportLauncherAsync(QObject* context, const std::string& id,
                                  std::function<void(StoreImportResult)> callback);
  static void OpenLauncherAsync(QObject* context, const std::string& id,
                                std::function<void(StoreActionResult)> callback);

  static bool ParseStoreEvent(const std::string& event_type, const std::string& data,
                              StoreEvent* out);
  // library.artwork_ready/_failed: kind "artwork", state "ready"/"failed",
  // error the failure's code.
  static bool ParseTitleArtworkEvent(const std::string& event_type, const std::string& data,
                                     StoreEvent* out);

  static bool ParseGameSummary(const std::string& data, GameSummary* out);
  // A games.updated payload's `games`.
  static bool ParseGameSummaries(const std::string& data, std::vector<GameSummary>* out);

  // A game.added payload's `open_config` (open_config_on_add).
  static bool ParseOpenConfig(const std::string& data);
  // game.added's `auto_install`: the scan runs this installer on its own.
  static bool ParseAutoInstall(const std::string& data);
  static bool ParseInstallerLeftover(const std::string& data, InstallerLeftoverEvent* out);
  // DELETE /v1/games/{id}/installer: the folder an install left its installer in.
  static void DeleteInstallerAsync(QObject* context, const std::string& id,
                                   std::function<void(GameActionResult)> callback);

  // Parses a `game.state` payload (`{"id", "state": "running" | "exited" |
  // "crashed", ...}`, docs/api.md) down to just id/state, enough to know
  // which row's Launch/Stop button to flip. Unlike game.added/updated this
  // carries no other game fields (not even play_seconds), so an
  // "exited"/"crashed" state is a signal to re-fetch, not something to patch
  // a row from directly.
  static bool ParseGameState(const std::string& data, GameStateEvent* out);

  // Parses `game.launched`. `tracked` defaults to false when the payload
  // omits it: an older daemon published this event only for the case where
  // nothing was watching, so that is what its silence meant.
  static bool ParseGameLaunched(const std::string& data, GameLaunchedEvent* out);
  static bool ParseInstallDetected(const std::string& data, InstallDetectedEvent* out);

  // Parses `game.removed`'s payload (`{"id": "..."}`, Server.cpp).
  static std::string ParseRemovedId(const std::string& data);
  // Parses `games.removed`'s payload (`{"ids": [...]}`).
  static std::vector<std::string> ParseRemovedIds(const std::string& data);

  // Parses a `game.metadata_ready`/`.metadata_failed` payload. `state` is
  // the event type, which the payload does not repeat; false if `data` is
  // not a JSON object with an id.
  static bool ParseMetadataEvent(const std::string& data, MetadataEvent* out);

  // Parses a `game.artwork_selected`/`.artwork_select_failed` payload.
  static bool ParseArtworkSelectEvent(const std::string& data, ArtworkSelectEvent* out);
  // Parses a `game.artwork_candidates_ready` payload.
  static bool ParseArtCandidatesEvent(const std::string& data, ArtCandidatesEvent* out);
  // Parses a `game.artwork_thumbs_ready` payload.
  static bool ParseArtThumbsEvent(const std::string& data, ArtThumbsEvent* out);

  // Parses a `notification` payload (`{"level": "...", "message": "..."}`).
  // False if `data` is not a JSON object with a message.
  static bool ParseNotification(const std::string& data, NotificationEvent* out);

  // Parses a `runners.download.started`/`.finished`/`.failed` payload.
  // `state` comes from the event type, which the payload itself doesn't
  // repeat.
  static bool ParseRunnerDownload(const std::string& event_type, const std::string& data,
                                  RunnerDownloadEvent* out);

  // Parses a `tricks.started`/`.finished`/`.failed` payload. `state` comes
  // from the event type, prefix-matched the same way ParseRunnerDownload is.
  static bool ParseTricksEvent(const std::string& event_type, const std::string& data,
                               TricksEvent* out);
};

}  // namespace mira_gui
