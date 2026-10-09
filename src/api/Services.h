#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <json.hpp>

#include "api/EventBus.h"
#include "api/Jobs.h"
#include "config/Config.h"
#include "core/Lane.h"
#include "core/Result.h"
#include "library/Catalog.h"
#include "library/FolderTags.h"
#include "library/UnclearMoves.h"
#include "metadata/FetchQueue.h"
#include "proc/ProcessSupervisor.h"
#include "runner/Downloader.h"
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

private:
  // Prefix program folders as each running Windows game launched, for CheckForInstall.
  // Before `supervisor`, whose watcher threads use it.
  std::mutex install_watch_mutex_;
  std::map<std::string, std::set<std::filesystem::path>> install_watch_;

public:
  proc::ProcessSupervisor supervisor;
  metadata::FetchQueue fetches{games.Metadata()};
  Lane tricks{"tricks", 1};  // one at a time: winetricks runs overlap badly in one prefix
  Lane artwork_selects{"artwork-select", 2};
  Lane artwork_thumbs{"artwork-thumbs", 4};
  Lane operations{"operations", 4};  // installs and downloads
  Lane installs{"installs", 1};      // installers a scan runs on its own
  library::CatalogCache catalogs;
  Lane catalog_checks{"catalog-checks", 2};  // after `catalogs`, so its tasks stop first
  // Folders scans found that could be any of several games moved by hand.
  library::UnclearMoves unclear_moves{events};
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
  // Sets `stopping` and wakes whatever waits on it, so shutdown doesn't wait out a poll or a stream.
  void BeginStopping();

  // The settings a record is built from, read once by a caller building many records at once.
  struct RecordSettings {
    double threshold;  // detect.low_confidence_threshold
    library::SortRules rules;
  };
  RecordSettings CurrentRecordSettings() const;
  // A game as the API shows it: model::ToJson plus `running`, `art`, `needs_check` and its
  // sorting (`sort_root`, `folder_tags`, `folder`).
  nlohmann::json Record(const model::Game& game, const RecordSettings* settings = nullptr);
  // Adds those fields to a model::ToJson record.
  void Decorate(nlohmann::json& record);
  void AddRecordFields(nlohmann::json& record, const model::Game& game, const RecordSettings& settings);

  // Rewrites the application menu entries to match the library.
  void SyncDesktopEntries();
  // After settings.toml is saved: applies a hand edit and tells clients, or notifies that it doesn't parse.
  void ReloadSettings();
  // The same for one game, after a change to just that game (also once it is deleted).
  void SyncDesktopEntry(const std::string& game_id);
  // Fetches metadata and art for games added outside a request.
  void QueueMetadata(const std::vector<model::Game>& games);
  // What every import and scan ends with: menu entries, then metadata for what it added.
  void AfterImport(const std::vector<model::Game>& added);

  // Closes out any session a previous mirad left behind and tells clients those games are idle.
  void ReconcileSessions();

  // Remembers the prefix program folders a launch started with; CheckForInstall compares at exit.
  void WatchForInstall(const std::string& game_id, std::set<std::filesystem::path> folders);

  // Runs `work` as a job and answers 202 {status, job}. The request's ?job=
  // names the job, so its client can listen for it before this reply lands.
  // `lane` is where the work runs; the jobs' own lane when null.
  void StartJob(const httplib::Request& req, httplib::Response& res, const std::string& kind,
                const std::string& target, const std::string& label, JobRegistry::Work work, Lane* lane = nullptr);
  // Queues a full metadata fetch for each game and reports as each finishes,
  // for a job: {refreshed, failed} once all have.
  Result<nlohmann::json> RefreshMetadata(std::vector<model::Game> games, JobRegistry::Progress& progress);
  // Sets up a Windows game's prefix as a job: it can take minutes (umu fetches its runtime the
  // first time). The caller stores the game `setting_up` first; game.updated says how it went.
  void ProvisionLater(const model::Game& game);
  // Claims a game for `purpose` ("launched", "moved", "deleted") until the result is dropped,
  // so it can't launch while its files change, or change while it launches or runs.
  Result<proc::ProcessSupervisor::Reservation> Claim(const std::string& game_id,
                                                     const std::string& purpose);
  // Deletes what DELETE /v1/games/{id} was asked to, before the game itself is removed. `staying`
  // are the games that remain afterwards: a folder one of them also uses isn't deleted. The
  // caller holds the game's Claim.
  Result<void> DeleteGameData(const model::Game& game, std::span<const model::Game> staying,
                              bool files, bool prefix, bool metadata);
  // Installs a runner build as a job, publishing runners.download.* as well.
  // With `replacing` ("kind:name"), games and the default using it move over.
  void InstallRunner(const httplib::Request& req, httplib::Response& res, const std::string& kind,
                     const std::string& source, const runner::ReleaseAsset& asset, const std::string& replacing);

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

  // Picks up games started outside Mira (the Steam client, a running launcher) so they show as playing.
  void StartExternalWatch();

private:
  void WatchExternalGames();
  // After a launched game exits: publishes game.install_detected when the run
  // added a program folder to its prefix, i.e. the "game" was an installer.
  void CheckForInstall(const std::string& game_id);

  std::mutex stop_mutex_;
  std::condition_variable stop_wake_;  // wakes WatchExternalGames as soon as `stopping` is set
  std::thread external_watch_;
};

}  // namespace mira::api
