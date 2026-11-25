#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <future>
#include <json.hpp>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "api/EventBus.h"
#include "api/HiddenLock.h"
#include "api/Jobs.h"
#include "config/Config.h"
#include "core/Lane.h"
#include "core/Result.h"
#include "display/Display.h"
#include "downloads/Queue.h"
#include "library/Catalog.h"
#include "library/Drops.h"
#include "library/FolderTags.h"
#include "library/FoundArchives.h"
#include "library/ImportSummary.h"
#include "library/Scanner.h"
#include "library/UnclearMoves.h"
#include "metadata/FetchQueue.h"
#include "proc/ProcessSupervisor.h"
#include "runner/Downloader.h"
#include "saves/Cloud.h"
#include "saves/remote/RemoteSync.h"
#include "store/GameStore.h"

namespace httplib {
struct Request;
struct Response;
}  // namespace httplib

namespace mira::api {

// Everything the daemon's routes act on: the stores, the supervisor and the
// lanes background work runs on. Built once by mirad (and by tests) and shared
// by the Server, the scanner and the watcher, so each of them finishes an
// import or a scan the same way.
class Services {
 public:
  Services(config::Config& config, store::GameStore& games, EventBus& events);
  ~Services();
  Services(const Services&) = delete;
  Services& operator=(const Services&) = delete;

  config::Config& config;
  store::GameStore& games;
  EventBus& events;
  HiddenLock hidden{games};
  // The folder the socket is in, set by Server::Serve: state that ends with the login session.
  std::filesystem::path runtime_dir;

  // While hidden games are locked: whether to answer as if this game, or the game or store title
  // with this id ("<source>-<ref>" for a title), weren't there.
  bool Conceals(const model::Game& game) const;
  // `id` may also be a path on disk.
  bool Conceals(const std::string& id) const;
  // Locks hidden games, blanks buffered events about them and publishes hidden.changed; nothing
  // when they're locked already.
  void LockHidden();
  // {password, locked}: GET /v1/hidden's answer and hidden.changed's payload.
  nlohmann::json HiddenState() const;
  void PublishHiddenState();

 private:
  std::mutex scan_mutex_;
  std::shared_future<library::ScanSummary> scan_;  // the running scan's, guarded by scan_mutex_

 public:
  proc::ProcessSupervisor supervisor;
  metadata::FetchQueue fetches{games.Metadata()};
  Lane tricks{"tricks", 1};  // one at a time: winetricks runs overlap badly in one prefix
  Lane artwork_selects{"artwork-select", 2};
  Lane artwork_thumbs{"artwork-thumbs", 4};
  Lane operations{"operations", 4};  // installs and downloads
  Lane installs{"installs", 1};      // installers a scan runs on its own
  Lane achievements{"achievements", 2};
  Lane achievement_icons{"achievement-icons", 4};
  Lane saves{"saves", 1};  // the save-location list's updates
  library::CatalogCache catalogs;
  Lane catalog_checks{"catalog-checks", 2};  // after `catalogs`, so its tasks stop first
  downloads::Queue downloads{config, games, events};
  display::Switcher display{games};
  // Folders scans found that could be any of several games moved by hand.
  library::UnclearMoves unclear_moves{events};
  // What was dropped into a Mira folder that Mira couldn't sort itself.
  library::Drops drops{config, games, events};
  // The archives in library and Mira folders that wait for the user to say who extracts them.
  library::FoundArchives found_archives{events};
  // After everything a job's work touches, so it's joined first on the way down.
  JobRegistry jobs{events};

  // Store installs stopped by POST /v1/library/install/pause, by "<source>-<ref>". Their files
  // stay and installing again continues them. Kept in memory only.
  class PausedInstalls {
   public:
    void Add(const std::string& target, nlohmann::json install) {
      std::lock_guard lock(mutex_);
      installs_[target] = std::move(install);
    }
    bool Remove(const std::string& target) {
      std::lock_guard lock(mutex_);
      return installs_.erase(target) > 0;
    }
    bool Contains(const std::string& target) const {
      std::lock_guard lock(mutex_);
      return installs_.contains(target);
    }
    nlohmann::json List() const {
      std::lock_guard lock(mutex_);
      nlohmann::json out = nlohmann::json::array();
      for (const auto& [target, install] : installs_) out.push_back(install);
      return out;
    }

   private:
    mutable std::mutex mutex_;
    std::map<std::string, nlohmann::json> installs_;
  } paused_installs;

  // Called after a request changes library_roots, so the watcher can follow.
  std::function<void()> on_roots_changed;
  std::atomic<bool> stopping{false};  // checked by open event streams and long work
  // Sets `stopping` and wakes whatever waits on it, so shutdown doesn't wait out a poll or a
  // stream.
  void BeginStopping();

  // The settings a record is built from, read once by a caller building many records at once.
  struct RecordSettings {
    library::SortRules rules;
  };
  RecordSettings CurrentRecordSettings() const;
  // A game as the API shows it: model::ToJson plus `running`, `art` and its
  // sorting (`sort_root`, `folder_tags`, `folder`).
  nlohmann::json Record(const model::Game& game, const RecordSettings* settings = nullptr);
  // Adds those fields to a model::ToJson record.
  void Decorate(nlohmann::json& record);
  void AddRecordFields(nlohmann::json& record, const model::Game& game,
                       const RecordSettings& settings);

  // Rewrites the application menu entries to match the library.
  void SyncDesktopEntries();
  // After settings.toml is saved: applies a hand edit and tells clients, or notifies that it
  // doesn't parse.
  void ReloadSettings();
  // The same for one game, after a change to just that game (also once it is deleted).
  void SyncDesktopEntry(const std::string& game_id);
  // Puts a Steam game's picked art into Steam's own library (steam.sync_art), after a pick.
  void SyncSteamArt(const std::string& game_id);
  // Fetches metadata and art for games just added, which then get their tags from Steam.
  void QueueMetadata(const std::vector<model::Game>& games);
  // A new game's Steam tags that are already the user's tags become its own
  // (tags.steam_new_games), once its first fetch has stored them.
  void TagFromSteam(const std::string& id);
  // A full library scan, metadata queued for what it added. One asked for while another runs waits
  // for that one's summary instead of scanning again.
  library::ScanSummary Scan();
  // What every import and scan ends with: menu entries, then metadata for what it added.
  void AfterImport(const std::vector<model::Game>& added);
  // A source's import job's end: marks it added, announces that, then AfterImport. The job's
  // {added, updated}.
  nlohmann::json FinishImport(const std::string& source, const library::ImportSummary& summary);

  // Imports the games Steam has installed, for a job: the work behind POST /v1/steam/scan.
  Result<nlohmann::json> ScanSteam();
  // ScanSteam as a job of its own, unless one is running: Steam wrote an app manifest.
  void ScanSteamLater();

  // Closes out any session a previous mirad left behind and tells clients those games are idle.
  void ReconcileSessions();

  // After a program run in the game's prefix exits: publishes game.programs_found with what it
  // installed since `before` (from library::ProgramFolders), to offer as launch options.
  void OfferNewPrograms(const std::string& game_id, const std::string& by,
                        const std::set<std::filesystem::path>& before);

  // Runs `work` as a job and answers 202 {status, job}. The request's ?job=
  // names the job, so its client can listen for it before this reply lands.
  // `lane` is where the work runs; the jobs' own lane when null.
  void StartJob(const httplib::Request& req, httplib::Response& res, const std::string& kind,
                const std::string& target, const std::string& label, JobRegistry::Work work,
                Lane* lane = nullptr);
  // Queues a full metadata fetch for each game and reports as each finishes,
  // for a job: {refreshed, failed} once all have.
  Result<nlohmann::json> RefreshMetadata(std::vector<model::Game> games,
                                         JobRegistry::Progress& progress);
  // Sets up a Windows game's prefix as a job: it can take minutes (umu fetches its runtime the
  // first time). The caller stores the game `setting_up` first; game.updated says how it went.
  void ProvisionLater(const model::Game& game);
  // Claims a game for `purpose` ("launched", "moved", "deleted") until the result is dropped,
  // so it can't launch while its files change, or change while it launches or runs.
  Result<proc::ProcessSupervisor::Reservation> Claim(const std::string& game_id,
                                                     const std::string& purpose);
  // Deletes what DELETE /v1/games/{id} was asked to, before the game itself is removed. `staying`
  // are the games that remain afterwards: a folder one of them also uses isn't deleted. Fetched
  // info goes too, except a store game's (its Not installed entry shows it) and the art the user
  // picked, unless `forget`. Its save backups only with `backups`. The caller holds the game's
  // Claim.
  Result<void> DeleteGameData(const model::Game& game, std::span<const model::Game> staying,
                              bool files, bool prefix, bool forget, bool backups);
  // Opens Steam's own uninstall for a Steam game. The game stays until Steam has uninstalled it,
  // then leaves the library keeping its record, as if a Steam import found it gone.
  Result<void> UninstallThroughSteam(const model::Game& game);
  // Installs a runner build as a job, publishing runners.download.* as well.
  // With `replacing` ("kind:name"), games and the default using it move over.
  void InstallRunner(const httplib::Request& req, httplib::Response& res, const std::string& kind,
                     const std::string& source, const runner::ReleaseAsset& asset,
                     const std::string& replacing);
  // InstallRunner's work: downloads and installs `asset`, publishing runners.download.*.
  Result<nlohmann::json> DownloadRunner(const std::string& kind, const std::string& source,
                                        const runner::ReleaseAsset& asset,
                                        const std::string& replacing);
  // With no Wine or Proton build installed (a fresh install), downloads the newest GE-Proton so
  // Windows games run.
  void EnsureWindowsRunner();
  // Moves what names a removed build onto the build that runs in its place, with games.updated
  // for the games it moved.
  void RepointRemovedRunners();
  // Downloads Epic's EOS overlay in the background (a setup job for "epic") when Epic is signed in
  // and it's missing: an account set up before Mira fetched it with legendary gets it untouched.
  void EnsureEosOverlay();

  // Files a store wrote for one launch of `game_id` (Epic's ownership token), deleted once it
  // exits.
  void RemoveAfterExit(const std::string& game_id, std::vector<std::filesystem::path> files);

  // Starts the program `game`'s store shares between its running games (GOG's comet) unless it
  // runs already, with its output in logs/<name>.log. Stopped when the last of them exits.
  void StartShared(const model::Game& game, const Command& command);
  // Stops it once no game of `game_id`'s store is running.
  void StopSharedIfIdle(const std::string& game_id);

  // Fetches a game's achievements from its store and caches them, publishing
  // game.achievements_ready when they changed. {id, total, unlocked, changed}.
  Result<nlohmann::json> RefreshAchievements(const std::string& id);
  // After a session ends: RefreshAchievements on the achievements lane, unless achievements are
  // off or the game was found to have none within the last week.
  void RefreshAchievementsAfterPlay(const std::string& id);

  // Updates the save-location list (saves::UpdateManifest) as a job on the saves lane, or finds
  // the update already running. The job's id.
  std::string UpdateSaveManifest(const std::string& requested_job = std::string());
  // UpdateSaveManifest when the list is due (saves::ManifestDue): what anything needing the list
  // calls, so it's fetched on first use and then at most daily, never on a timer.
  void CheckSaveManifest();
  // Snapshots a game's saves (saves::TakeSnapshot) holding its claim, publishing
  // game.saves_changed when one was taken. Errs game_running or game_busy as Claim does.
  Result<nlohmann::json> BackUpSaves(const std::string& id, std::string_view reason);
  // Puts a snapshot's files back holding the game's claim, after a before_restore snapshot of
  // what's there now. {restored, files, undo}: `undo` is the snapshot holding what was replaced
  // (null when there were no saves). Errs snapshot_not_found and saves::RestoreSnapshot's errors.
  Result<nlohmann::json> RestoreSaves(const std::string& id, const std::string& snapshot);
  // SyncCloud holding the game's claim.
  Result<nlohmann::json> SyncCloudSaves(const std::string& id, saves::CloudAction action);
  // Whether `game`'s saves may be synced with the user's own remote: one is connected and no
  // store's cloud keeps them.
  bool RemoteEligible(const model::Game& game) const;
  // RemoteEligible, and not turned off for the game.
  bool RemoteSyncs(const model::Game& game) const;
  // The connected remotes and the uploads they're owed, without hidden games while those are
  // locked: {primary, mirror, rclone, pending}.
  nlohmann::json RemoteState();
  // Syncs every game RemoteSyncs, reporting each: {synced, failed}. A game in use is left out.
  Result<nlohmann::json> SyncAllRemote(JobRegistry::Progress& progress);
  // What's being done with a game's saves now: "backing_up", "restoring", "syncing", or "".
  std::string SavesDoing(const std::string& id);
  // Blocks until nothing is being done with the game's saves, so a launch right after a session
  // waits for its backup and sync; that work ends on its own. False when mirad began stopping.
  bool WaitForSavesWork(const std::string& id);
  // Before a launch, under its claim: a first backup for a game that has none, then for a game
  // whose cloud saves are synced, a sync that gets saves.cloud_timeout_s. `choice` settles a
  // conflict ("keep_local", "use_cloud") or skips the sync ("skip"). Errs only saves_conflict:
  // a cloud that can't be reached in time lets the game start with the saves here.
  Result<void> SavesBeforeLaunch(const model::Game& game, std::string_view choice);
  // After a session ends, on the saves lane: the after-play backup, keeping a failure for
  // GET /v1/games/{id}/saves to show, then a cloud sync; a conflict found then is a persistent
  // notification. `then` runs once they're done, so a move of the game's folder can't take
  // their claim first.
  void SavesAfterPlay(const std::string& id, std::function<void()> then);

  // After settings changed, however (a request, a reset, a hand edit of settings.toml): syncs the
  // menu entries, runs SortingChanged when sorting's settings are among `keys`, and publishes
  // config.changed with the keys (names only: values can be secrets) and the frontend table, so a
  // client applies another client's change without asking again.
  void SettingsChanged(const std::vector<std::string>& keys);
  // After library_roots, tags.folders or tags.sorted_roots changed: watches the sorting folders,
  // tells clients every record changed (sort_root, folder_tags) and moves what's out of place.
  void SortingChanged();
  // Moves each game's folder to where its tags put it (tags.folders), as one job when any
  // has to move; nothing when none does. A game that can't be claimed (running, being deleted) is
  // left for later: its exit sorts it again.
  void SortByTags(std::vector<std::string> ids);
  void SortAllByTags();
  // One games.updated with every record, after a setting that changes what records say.
  void PublishAllGames();

  // Picks up games started outside Mira (the Steam client, a running launcher) so they show as
  // playing.
  void StartExternalWatch();

 private:
  // Marks what's being done with a game's saves (SavesDoing) for as long as it lives, publishing
  // game.saves_activity {id, doing} as it starts and as it ends; overlapping work shows the newest.
  class SavesWork {
   public:
    SavesWork(Services& services, std::string id, std::string doing);
    ~SavesWork();
    SavesWork(const SavesWork&) = delete;
    SavesWork& operator=(const SavesWork&) = delete;

   private:
    Services& services_;
    std::string id_;
  };
  // saves::RunCloud, or RunRemote when RemoteEligible, with a before_cloud_download snapshot before
  // anything local is overwritten, once `before_download` agreed. Publishes game.saves_changed when
  // done.
  Result<nlohmann::json> SyncCloud(const model::Game& game, saves::CloudAction action,
                                   const std::function<Result<void>()>& before_download);
  // Whether `game`'s saves are synced with its store's cloud, as far as is known without asking.
  bool CloudSyncs(const model::Game& game) const;
  // Copies the game's saves to the mirror, on the saves lane.
  void MirrorSaves(const std::string& id);
  // This computer as the remote's revisions name it, made up on first use.
  saves::remote::Device RemoteDevice();
  // Notes that `game`'s saves are owed to the remote, so a failed upload is tried again.
  void OweRemote(const model::Game& game);
  // One notification per outage that saves wait to be uploaded.
  void NoteRemote(const model::Game& game, const Result<nlohmann::json>& synced);
  // Every 15 minutes, and first at startup: tries the uploads still owed, on the saves lane.
  void RetryRemoteSyncs();
  // SavesBeforeLaunch's sync, stopped after saves.cloud_timeout_s unless it's downloading.
  Result<void> SyncBeforeLaunch(const model::Game& game, std::string_view choice);
  // The after-play backup, keeping its failure for the game's saves.
  void BackUpAfterPlay(const model::Game& game);
  // A persistent notification of a cloud save conflict found `when`, with its fix, for a launch
  // from outside the GUI or one found after play.
  void NotifySavesConflict(const model::Game& game, const Error& error, std::string_view when);
  void WatchExternalGames();
  // Locks hidden games once hidden.lock_after_minutes pass with no one using Mira and no game
  // running.
  void LockHiddenWhenIdle();
  // EventBus's filter: while locked, drops or rewrites events about concealed games.
  bool FilterEvent(std::string& type, nlohmann::json& payload, bool live);

  // Each running job's target, so its later events (which carry only the job's id) can be
  // concealed.
  std::mutex job_targets_mutex_;
  std::map<std::string, std::string> job_targets_;

  // After library_roots changed: games found in a library folder that was taken out are marked
  // missing, as if their drive were unplugged. Adding the folder back (or the same games in another
  // folder, such as a drive mounted elsewhere) finds them again with their history.
  void MarkLeftRootsMissing();
  std::mutex roots_mutex_;
  std::vector<std::filesystem::path> library_roots_;  // as of the last MarkLeftRootsMissing

  std::mutex save_manifest_mutex_;  // so two requests don't both start an update
  // Games looked at for a first backup this run, so later launches skip the survey.
  std::mutex first_backups_mutex_;
  std::set<std::string> first_backups_;
  // Held from a change to saves_doing_ through its game.saves_activity, so clients get them in
  // order: a launch's work starting can't be announced before the previous work's end.
  std::mutex saves_activity_mutex_;
  std::mutex saves_doing_mutex_;
  // Each game's SavesWork and what it does, newest last.
  std::map<std::string, std::vector<std::pair<const SavesWork*, std::string>>> saves_doing_;
  std::condition_variable saves_done_;  // a game's saves_doing_ emptied
  std::mutex remote_mutex_;
  bool remote_failing_ = false;  // an outage the user was told of
  std::chrono::steady_clock::time_point remote_retry_at_{};
  std::mutex exit_files_mutex_;
  std::map<std::string, std::vector<std::filesystem::path>> exit_files_;

  // Steam games handed to Steam's own uninstall, by id, with when to stop waiting for it.
  std::mutex steam_uninstalls_mutex_;
  std::map<std::string, std::chrono::steady_clock::time_point> steam_uninstalls_;
  // Removes, keeping their records, the handed-over games Steam no longer has installed. True
  // while any are still waited for.
  bool CheckSteamUninstalls();

  std::mutex stop_mutex_;
  std::condition_variable stop_wake_;  // wakes WatchExternalGames as soon as `stopping` is set
  std::thread external_watch_;
};

}  // namespace mira::api
