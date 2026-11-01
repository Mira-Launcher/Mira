#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#endif

namespace mira::library {

// What happens in the folders Watcher watches: inotify on Linux, FSEvents on macOS. Each folder is
// watched on its own, not recursively.
class DirEvents {
 public:
  enum class Kind {
    kAdded,      // created, or moved in
    kRemoved,    // deleted
    kMovedAway,  // moved out, or renamed
    kWritten,    // a file written in place, only in folders added with `writes`
    kGone,       // the watched folder itself went away
    kOverflow,   // events were dropped: anything may have changed
  };
  struct Change {
    int wd = -1;
    std::string name;  // the entry in the watched folder
    Kind kind;
  };
  struct Fired {
    bool stop = false;
    bool reload = false;
    bool timer = false;
    std::vector<Change> changes;
  };

  DirEvents();
  ~DirEvents();
  DirEvents(const DirEvents&) = delete;
  DirEvents& operator=(const DirEvents&) = delete;

  bool Ok() const;
  // A watch on `dir`'s entries, or -1. The same folder gives the same descriptor.
  int Add(const std::filesystem::path& dir, bool writes = false);
  void Remove(int wd);
  // Fires the timer once `ms` from now, or never with nullopt.
  void ArmTimer(std::optional<int> ms);
  // Both safe from any thread.
  void WakeStop();
  void WakeReload();
  // Blocks until something fires. False on an error it can't go on from.
  bool Wait(Fired& fired);

#ifdef __APPLE__
  // FSEvents' callback, on its own queue.
  void OnEvents(size_t count, char** paths, const std::uint32_t* flags);
#endif

 private:
#ifdef __linux__
  int inotify_fd_ = -1;
  int epoll_fd_ = -1;
  int timer_fd_ = -1;
  int stop_fd_ = -1;
  int reload_fd_ = -1;
  void ReadInotify(std::vector<Change>& changes);
#elif defined(__APPLE__)
  struct Dir {
    std::string path;  // resolved, as FSEvents reports it
    bool writes = false;
  };
  void Restart();

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Change> queue_;
  bool stop_ = false;
  bool reload_ = false;
  std::optional<std::chrono::steady_clock::time_point> timer_;
  std::map<int, Dir> dirs_;
  int next_wd_ = 1;
  bool dirty_ = false;
  void* stream_ = nullptr;        // FSEventStreamRef
  void* queue_handle_ = nullptr;  // dispatch_queue_t
#endif
};

}  // namespace mira::library
