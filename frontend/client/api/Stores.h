#pragma once

#include <QImage>
#include <QObject>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "../Types.h"

// Stores, launchers and the sources they make up.
// Each call runs on a worker thread and delivers its result on the main thread
// (client/Async.h), so the UI never blocks on the socket. See docs/api.md.
namespace mira_gui::api {

// GET /v1/sources: every source's state, in sidebar order.
void ListSourcesAsync(QObject* context, std::function<void(SourcesResult)> callback);
// PATCH /v1/sources/{id}: only the fields given change.
void PatchSourceAsync(QObject* context, const std::string& id, std::optional<bool> enabled,
                      std::optional<bool> in_sidebar, std::function<void(PatchConfigResult)> callback);
// PATCH /v1/sources/{id} {"added": true}: set up, which also turns it on.
void AddSourceAsync(QObject* context, const std::string& id, std::function<void(PatchConfigResult)> callback);
// PUT /v1/sources/order: the sidebar order, every source id first to last.
void SetSourceOrderAsync(QObject* context, const std::vector<std::string>& ids,
                         std::function<void(PatchConfigResult)> callback);

void GetStoreStatusAsync(QObject* context, const std::string& source,
                         std::function<void(StoreStatusResult)> callback);

// Downloads the store's helper tool as a job: the callback runs when it has finished or failed.
void SetupStoreToolAsync(QObject* context, const std::string& source,
                         std::function<void(StoreActionResult)> callback);

// `credential` is whatever the user pasted: a code, a whole login page or
// redirect URL, an API key, or a session cookie, per store.
void SignInStoreAsync(QObject* context, const std::string& source, const std::string& credential,
                      std::function<void(StoreActionResult)> callback);

void SignOutStoreAsync(QObject* context, const std::string& source,
                       std::function<void(StoreActionResult)> callback);

void ImportStoreAsync(QObject* context, const std::string& source,
                      std::function<void(StoreImportResult)> callback);

// GET /v1/library?source=: owned titles, installed or not, as mirad last stored them (it re-checks
// behind). An empty source lists every store's; `fresh` waits for each store's own answer.
void GetStoreLibraryAsync(QObject* context, const std::string& source, bool fresh,
                          std::function<void(StoreLibraryResult)> callback);

void InstallStoreTitleAsync(QObject* context, const std::string& source, const std::string& ref,
                            bool update, std::function<void(StoreActionResult)> callback);

// POST /v1/library/install/pause: stops a running Epic/GOG/Amazon install, keeping its files.
void PauseStoreInstallAsync(QObject* context, const std::string& source, const std::string& ref,
                            std::function<void(StoreActionResult)> callback);
// DELETE /v1/library/install/paused: forgets a paused install.
void DiscardPausedInstallAsync(QObject* context, const std::string& source, const std::string& ref,
                               std::function<void(StoreActionResult)> callback);
// GET /v1/library/install/paused.
void ListPausedInstallsAsync(QObject* context, std::function<void(PausedInstallsResult)> callback);

// POST /v1/library/artwork: fetch covers for these titles, one at a time.
// Each one ends in a library.artwork_ready/_failed event.
void QueueTitleArtworkAsync(QObject* context, const std::string& source,
                            std::vector<StoreTitle> titles,
                            std::function<void(StoreActionResult)> callback);

// GET /v1/sources/{id}/removal and POST /v1/sources/{id}/remove.
void GetRemovalPlanAsync(QObject* context, const std::string& source,
                         std::function<void(RemovalPlanResult)> callback);

void RemoveSourceAsync(QObject* context, const std::string& source,
                       std::function<void(RemoveSourceResult)> callback);

// GET/POST /v1/sources/{id}/runner: a store's default runner, or a launcher's prefix runner.
void GetSourceRunnerAsync(QObject* context, const std::string& source,
                          std::function<void(SourceRunnerResult)> callback);

void SetSourceRunnerAsync(QObject* context, const std::string& source,
                          const std::string& runner_ref, bool apply_to_games,
                          std::function<void(SourceRunnerResult)> callback);

// GET/POST /v1/stores/itch/collections, DELETE /v1/stores/itch/collections/{id}.
void GetItchCollectionsAsync(QObject* context, std::function<void(ItchCollectionsResult)> callback);

void AddItchCollectionAsync(QObject* context, const std::string& link,
                            std::function<void(StoreActionResult)> callback);

void RemoveItchCollectionAsync(QObject* context, std::int64_t id,
                               std::function<void(StoreActionResult)> callback);

void GetHumbleLibraryAsync(QObject* context, std::function<void(HumbleLibraryResult)> callback);

// A job: the callback runs when the download has finished or failed (`nothing_to_download` for a
// key-only bundle).
void DownloadHumbleBundleAsync(QObject* context, const std::string& bundle_key,
                               std::function<void(HumbleDownloadResult)> callback);

// The page to sign in at. Amazon's is made per attempt (PKCE), not a fixed page.
void BeginStoreLoginAsync(QObject* context, const std::string& source,
                          std::function<void(LoginUrlResult)> callback);

// The credential in text the user copied (mirad knows each store's shape), empty when there's none.
void FindStoreCredentialAsync(QObject* context, const std::string& source, const std::string& text,
                              std::function<void(FoundCredentialResult)> callback);

void GetSteamAccountsAsync(QObject* context, std::function<void(SteamAccountsResult)> callback);
void GetSteamInstalledAsync(QObject* context, std::function<void(SteamInstalledResult)> callback);

void GetLaunchersAsync(QObject* context, std::function<void(LaunchersResult)> callback);

// Detached; a StoreEvent with kind "setup" and the launcher's id follows.
void InstallLauncherAsync(QObject* context, const std::string& id,
                          std::function<void(StoreActionResult)> callback);

// POST /v1/launchers/office/apps: adds these Microsoft 365 apps, now or once its running install
// ends. Detached; a launcher.install.failed event says when adding them failed.
void AddOfficeAppsAsync(QObject* context, const std::vector<std::string>& refs,
                        std::function<void(StoreActionResult)> callback);

void ImportLauncherAsync(QObject* context, const std::string& id,
                         std::function<void(StoreImportResult)> callback);

void OpenLauncherAsync(QObject* context, const std::string& id,
                       std::function<void(StoreActionResult)> callback);

}  // namespace mira_gui::api
