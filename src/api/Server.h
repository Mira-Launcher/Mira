#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "api/EventBus.h"
#include "api/Jobs.h"
#include "config/Config.h"
#include "core/BackgroundQueue.h"
#include "core/Result.h"
#include "metadata/ArtIndex.h"
#include "metadata/FetchQueue.h"
#include "proc/ProcessSupervisor.h"
#include "runner/Downloader.h"
#include "store/GameStore.h"

// httplib::Server is used only by Server.cpp; forward-declared here so
// including this header does not pull the whole vendored library into every
// translation unit that wires up a daemon.
namespace httplib {
class Server;
struct Request;
struct Response;
}

namespace mira::api {

// The REST-over-UDS surface described in docs/api.md. Owns the socket and the
// httplib server; everything it touches (Config, GameStore, EventBus) is
// injected so the routes can be exercised without a real socket in tests.
class Server {
public:
  Server(config::Config& config, store::GameStore& games, EventBus& events);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds the UDS socket (removing a stale one first) and blocks serving
  // until Stop() is called from another thread. The bind happens inside this
  // call so a failure is reported through the return value rather than a
  // separate two-step API.
  Result<void> Serve(const std::filesystem::path& socket_path);
  void Stop();

  // Closes out any session a previous mirad left behind (see
  // proc::ProcessSupervisor::Reconcile), called once at startup, before
  // Serve(), so a session already re-adopted is visible to the very first
  // GET /v1/games a client makes.
  void ReconcileSessions();

  // Called after a request changes library_roots, so the watcher can follow.
  // Set once, before Serve().
  // Queues metadata and art for games added outside a request (the startup scan).
  void QueueMetadata(const std::vector<model::Game>& games);
  metadata::FetchQueue& MetadataQueue() { return metadata_fetches_; }

  void SetOnLibraryRootsChanged(std::function<void()> callback) { on_roots_changed_ = std::move(callback); }

private:
  void RegisterRoutes();
  void WatchExternalGames();
  void BeginStopping();
  // Installs a runner build in the background, publishing runners.download.*.
  // With `replacing` ("kind:name"), games and the default using it move over.
  void InstallRunnerAsync(const std::string& kind, const std::string& source, const runner::ReleaseAsset& asset,
                          const std::string& replacing);
  // Runs `work` as a job and answers 202 {status, job}. The request's ?job=
  // names the job, so its client can listen for it before this reply lands.
  void StartJob(const httplib::Request& req, httplib::Response& res, const std::string& kind,
                const std::string& target, const std::string& label, JobRegistry::Work work);
  // Queues a full metadata fetch for each game and reports as each finishes,
  // for a job: {refreshed, failed} once all have.
  Result<nlohmann::json> RefreshMetadata(std::vector<model::Game> games, JobRegistry::Progress& progress);
  // Deletes what DELETE /v1/games/{id} was asked to, before the game itself is removed.
  Result<void> DeleteGameData(const model::Game& game, bool files, bool prefix, bool metadata);
  // A game as the API shows it: model::ToJson plus `running` and `art`.
  nlohmann::json Record(const model::Game& game);
  // After a launched game exits: publishes game.install_detected when the run
  // added a program folder to its prefix, i.e. the "game" was an installer.
  void CheckForInstall(const std::string& game_id);

  config::Config& config_;
  store::GameStore& games_;
  EventBus& events_;
  std::unique_ptr<httplib::Server> http_;
  // Prefix program folders as each running Windows game launched, for
  // CheckForInstall. Before supervisor_, whose watcher threads use it.
  std::mutex install_watch_mutex_;
  std::map<std::string, std::set<std::filesystem::path>> install_watch_;
  proc::ProcessSupervisor supervisor_;
  metadata::ArtIndex art_index_{config_};
  metadata::FetchQueue metadata_fetches_;
  BackgroundQueue tricks_queue_;
  BackgroundQueue artwork_selects_;
  BackgroundQueue artwork_thumbs_;
  BackgroundQueue operations_;  // installs and downloads; joined with the Server so none outlive it
  // After everything a job's work touches, so it's joined first on the way down.
  JobRegistry jobs_{events_};
  std::function<void()> on_roots_changed_;
  std::atomic<bool> stopping_{false};  // checked by open SSE connections; see EventBus::WaitNext
  std::mutex stop_mutex_;
  std::condition_variable stop_wake_;  // wakes WatchExternalGames as soon as stopping_ is set
  std::thread external_watch_;
};

}  // namespace mira::api
