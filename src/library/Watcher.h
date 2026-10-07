#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
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
// One inotify watch per root and per sorting folder in it (.hidden, a folder
// tag's folder), non-recursive. A new directory is debounced
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
  // How many times Run() has put watches on library_roots: 1 once it first is, one more after each
  // ReloadRoots(). A change made after it rises is seen.
  int RootsWatched() const { return roots_watched_; }

  // Fetch through `queue` instead of a queue of its own, so one set of workers and one dedupe serve everything.
  // Before Run().
  void UseMetadataQueue(metadata::FetchQueue& queue) { metadata_fetches_ = &queue; }
  // Runs on the watcher's thread when settings.toml is saved, by hand or by Mira. Before Run().
  void OnSettingsSaved(std::function<void()> callback) { on_settings_saved_ = std::move(callback); }
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
  // Watches `dir` for changes in `root`, and the sorting folders inside it (FolderTags.h).
  void WatchFolder(const std::filesystem::path& root, const std::filesystem::path& dir);
  // Drops the watches on `dir` and the sorting folders inside it, never a root's.
  void Unwatch(const std::filesystem::path& dir);
  // Whether `dir` is a sorting folder under `root` rather than a game of that name.
  bool IsSortingFolder(const std::filesystem::path& root, const std::filesystem::path& dir) const;

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
  int settings_wd_ = -1;  // settings.toml's folder
  std::function<void()> on_settings_saved_;

  struct Watched {
    std::filesystem::path root;
    std::filesystem::path dir;  // the root, or a sorting folder in it
  };
  std::map<int, Watched> watches_;  // inotify watch descriptor -> what it watches
  std::atomic<int> roots_watched_{0};
  std::map<std::string, Pending> pending_;               // absolute dir path -> debounce state
};

}  // namespace mira::library
