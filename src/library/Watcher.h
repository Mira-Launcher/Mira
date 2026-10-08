#pragma once

#include <filesystem>
#include <map>

#include "api/EventBus.h"
#include "config/Config.h"
#include "metadata/FetchQueue.h"
#include "store/GameStore.h"

namespace mira::library {

// Creates each missing library root that is inside $HOME, so the defaults
// under ~/Mira work on a fresh install. A root elsewhere may be an unmounted
// drive, so it's left missing.
void CreateMissingRoots(const config::Config& config);

// Watches every enabled library root and rescans automatically: "drop a
// folder in and it's picked up" without running `mira scan` by hand.
//
// One inotify watch per root, non-recursive. A new directory is debounced
// (rescanned once its size is unchanged for `scan.debounce_ms`) rather than
// scanned mid-copy. epoll_wait blocks with no timeout except while a
// directory is being watched for size stability.
//
// Runs on its own thread (Run() blocks); Stop() unblocks it via an eventfd,
// safe to call from any other thread.
class Watcher {
public:
  Watcher(config::Config& config, store::GameStore& games, api::EventBus& events);
  ~Watcher();
  Watcher(const Watcher&) = delete;
  Watcher& operator=(const Watcher&) = delete;

  // Watches library_roots as they are when Run() starts; ReloadRoots()
  // re-reads them. Both Stop() and ReloadRoots() are safe from any thread.
  void Run();
  void Stop();
  void ReloadRoots();

  // Fetch through `queue` instead of a queue of its own, so one set of workers and one dedupe serve everything.
  // Before Run().
  void UseMetadataQueue(metadata::FetchQueue& queue) { metadata_fetches_ = &queue; }
  // Where a scan runs an installer it starts on its own. Before Run().
  void UseInstallLane(Lane& lane) { installs_ = &lane; }

private:
  struct Pending {
    std::filesystem::path root;
    std::uintmax_t last_size = 0;
    std::int64_t stable_since_ms = 0;
    bool is_archive = false;  // extract-and-remove on settle, instead of scanning it as a folder
  };

  void HandleInotify();
  void HandleDebounceTick();
  void ScheduleCheck(const std::filesystem::path& root, const std::filesystem::path& path, bool is_archive);
  void RearmTimer();
  // Drops every root watch and adds library_roots afresh. Run()'s thread only.
  void WatchRoots();

  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
  metadata::FetchQueue own_fetches_{games_.Metadata()};
  metadata::FetchQueue* metadata_fetches_ = &own_fetches_;
  Lane* installs_ = nullptr;

  int inotify_fd_ = -1;
  int epoll_fd_ = -1;
  int timer_fd_ = -1;
  int stop_fd_ = -1;
  int reload_fd_ = -1;

  std::map<int, std::filesystem::path> watch_to_root_;  // inotify watch descriptor -> root
  std::map<std::string, Pending> pending_;               // absolute dir path -> debounce state
};

}  // namespace mira::library
