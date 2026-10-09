#pragma once

#include <QImage>
#include <QObject>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../Types.h"

// One game's endpoints: launching, editing, its settings, log, installer and folders.
// Each call runs on a worker thread and delivers its result on the main thread
// (client/Async.h), so the UI never blocks on the socket. See docs/api.md.
namespace mira_gui::api {

// DELETE /v1/games/{id}[?delete_files=true][?delete_prefix=true][?delete_metadata=true].
// All opt-in; false/omitted never touches disk. delete_metadata alone has
// no library/prefix-root restriction: it's keyed by id under Mira's own state dir.
void DeleteGameAsync(QObject* context, const std::string& id, bool delete_files, bool delete_prefix,
                     bool delete_metadata, std::function<void(DeleteResult)> callback);

// POST /v1/games/{id}/launch. Returns once the process exists, not once
// it exits. 409 if the game isn't `ready` (message explains why, e.g.
// needs_install) or 400 if the runner reference doesn't resolve.
void LaunchGameAsync(QObject* context, const std::string& id,
                     std::function<void(LaunchResult)> callback);

// POST /v1/games/{id}/stop. SIGTERMs the game's process group; 409 if it
// isn't currently running.
void StopGameAsync(QObject* context, const std::string& id,
                   std::function<void(StopResult)> callback);

// GET /v1/games/{id}, for the detail/edit view.
void GetGameAsync(QObject* context, const std::string& id,
                  std::function<void(GameDetailResult)> callback);

// PATCH /v1/games/{id}. Setting any of these fields marks the game as reviewed.
void PatchGameAsync(QObject* context, const std::string& id, const GamePatch& patch,
                    std::function<void(PatchGameResult)> callback);

// POST /v1/games/{id}/run. Runs `exe_path` inside this game's prefix,
// provisioning one on demand, which is how a needs_install game's
// installer actually gets run, since Scanner never auto-provisions one.
void RunInPrefixAsync(QObject* context, const std::string& id, const std::string& exe_path,
                      const std::string& args, std::function<void(RunInPrefixResult)> callback);

// POST /v1/games/{id}/finish-install: flips a needs_install game to
// ready once exe_path points at whatever the installer produced. 409 if
// exe_path is still empty.
// With `install_path` and `exe_path`, first switches the game to that
// program installed in its prefix (game.install_detected).
void FinishInstallAsync(QObject* context, const std::string& id,
                        std::function<void(FinishInstallResult)> callback,
                        const std::string& install_path = std::string(),
                        const std::string& exe_path = std::string());

// GET /v1/games/{id}/config: this game's resolved settings, tagged by
// layer (see GameConfigEntry).
void GetGameConfigAsync(QObject* context, const std::string& id,
                        std::function<void(GameConfigResult)> callback);

// PATCH /v1/games/{id}/config. Same all-or-nothing validation as
// PATCH /v1/config (docs/api.md): only send edits that actually changed.
void PatchGameConfigAsync(QObject* context, const std::string& id,
                          const std::vector<GameConfigEdit>& edits,
                          std::function<void(PatchGameConfigResult)> callback);

// PATCH /v1/games: tags and overrides for many games in one request.
void PatchGamesAsync(QObject* context, const GamesPatch& patch,
                     std::function<void(PatchGamesResult)> callback);

// GET /v1/games/{id}/log?lines=. `lines` is how many trailing lines to ask
// for; an empty result means nothing has ever been logged, not a failure.
void GetGameLogAsync(QObject* context, const std::string& id, int lines,
                     std::function<void(GameLogResult)> callback);

// GET /v1/games/{id}/sessions?limit=. Newest first.
void GetGameSessionsAsync(QObject* context, const std::string& id, int limit,
                          std::function<void(GameSessionsResult)> callback);

// POST /v1/games/{id}/tricks. Returns 202; watch for
// tricks.started/.finished/.failed via ParseTricksEvent.
void RunWinetricksAsync(QObject* context, const std::string& id, const std::string& verb,
                        std::function<void(TricksResult)> callback);

// POST /v1/games/manual: adds a game record directly, for an installer or
// a folder outside every library root. Response mirrors GetGameAsync.
void AddManualGameAsync(QObject* context, const std::string& install_path,
                        const std::string& exe_path, const std::string& name,
                        const std::string& platform, bool is_installer,
                        std::function<void(GameDetailResult)> callback);

// The game's own installer, or `path` (absolute, or relative to its
// install folder) when choosing a different one.
void GetInstallerInfoAsync(QObject* context, const std::string& id, const std::string& path,
                           std::function<void(InstallerInfoResult)> callback);

// Detached; an InstallEvent follows. Empty `installer` uses the game's own.
void InstallGameAsync(QObject* context, const std::string& id, bool interactive,
                      const std::string& installer, std::function<void(GameActionResult)> callback);

void GetInstallProgressAsync(QObject* context, const std::string& id,
                             std::function<void(InstallProgressResult)> callback);

// POST /v1/games/{id}/relocate: moves one game's files to `install_path` and its prefix to
// `data_dir`. An empty one stays where it is.
void RelocateGameAsync(QObject* context, const std::string& id, const std::string& install_path,
                       const std::string& data_dir, std::function<void(GameDetailResult)> callback);

// POST /v1/games/delete: the same for many games in one request.
void DeleteGamesAsync(QObject* context, const std::vector<std::string>& ids, bool delete_files,
                      bool delete_prefix, bool delete_metadata,
                      std::function<void(DeleteGamesResult)> callback);

// DELETE /v1/games/{id}/installer: the folder an install left its installer in.
void DeleteInstallerAsync(QObject* context, const std::string& id,
                          std::function<void(GameActionResult)> callback);

}  // namespace mira_gui::api
