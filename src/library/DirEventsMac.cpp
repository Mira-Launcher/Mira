#ifdef __APPLE__

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <sys/stat.h>

#include <system_error>
#include <utility>

#include "library/DirEvents.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

void Callback(ConstFSEventStreamRef, void* self, size_t count, void* paths,
              const FSEventStreamEventFlags flags[], const FSEventStreamEventId ids[]) {
  static_cast<DirEvents*>(self)->OnEvents(count, static_cast<char**>(paths), flags, ids);
}

void Release(void*& stream) {
  if (stream == nullptr) return;
  auto* ref = static_cast<FSEventStreamRef>(stream);
  FSEventStreamStop(ref);
  FSEventStreamInvalidate(ref);
  FSEventStreamRelease(ref);
  stream = nullptr;
}

}  // namespace

DirEvents::DirEvents() {
  queue_handle_ = dispatch_queue_create("mira.watcher", DISPATCH_QUEUE_SERIAL);
  since_ = FSEventsGetCurrentEventId();
}

DirEvents::~DirEvents() {
  Release(stream_);
  if (queue_handle_ != nullptr) dispatch_release(static_cast<dispatch_queue_t>(queue_handle_));
}

bool DirEvents::Ok() const {
  return queue_handle_ != nullptr;
}

int DirEvents::Add(const fs::path& dir, bool writes) {
  std::error_code ec;
  const fs::path resolved = fs::canonical(dir, ec);
  if (ec || !fs::is_directory(resolved, ec)) return -1;
  const std::scoped_lock lock(mutex_);
  for (auto& [wd, watched] : dirs_) {
    if (watched.path == resolved.string()) {
      watched.writes = watched.writes || writes;
      return wd;
    }
  }
  const int wd = next_wd_++;
  dirs_[wd] = {.path = resolved.string(), .writes = writes};
  dirty_ = true;
  return wd;
}

void DirEvents::Remove(int wd) {
  const std::scoped_lock lock(mutex_);
  if (dirs_.erase(wd) > 0) dirty_ = true;
}

void DirEvents::ArmTimer(std::optional<int> ms) {
  const std::scoped_lock lock(mutex_);
  timer_.reset();
  if (ms) timer_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(*ms);
}

void DirEvents::WakeStop() {
  const std::scoped_lock lock(mutex_);
  stop_ = true;
  wake_.notify_all();
}

void DirEvents::WakeReload() {
  const std::scoped_lock lock(mutex_);
  reload_ = true;
  wake_.notify_all();
}

// A stream watches a fixed set of folders, so a changed set needs a new one. It starts from the
// last event the old one saw.
// dirs_ only changes on Wait()'s thread, so it's read here unlocked.
void DirEvents::Restart(std::uint64_t since) {
  Release(stream_);
  if (dirs_.empty()) return;
  CFMutableArrayRef paths =
      CFArrayCreateMutable(nullptr, CFIndex(dirs_.size()), &kCFTypeArrayCallBacks);
  for (const auto& [wd, watched] : dirs_) {
    CFStringRef path =
        CFStringCreateWithCString(nullptr, watched.path.c_str(), kCFStringEncodingUTF8);
    CFArrayAppendValue(paths, path);
    CFRelease(path);
  }
  FSEventStreamContext context{};
  context.info = this;
  FSEventStreamRef stream =
      FSEventStreamCreate(nullptr, &Callback, &context, paths, since, 0.05,
                          kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
                              kFSEventStreamCreateFlagWatchRoot);
  CFRelease(paths);
  if (stream == nullptr) return;
  FSEventStreamSetDispatchQueue(stream, static_cast<dispatch_queue_t>(queue_handle_));
  FSEventStreamStart(stream);
  stream_ = stream;
}

bool DirEvents::Wait(Fired& fired) {
  fired = {};
  std::unique_lock lock(mutex_);
  if (std::exchange(dirty_, false)) {
    const std::uint64_t since = since_;
    // Not under the lock: stopping the stream can wait for a callback, which takes it.
    lock.unlock();
    Restart(since);
    lock.lock();
  }
  const auto ready = [&] {
    return stop_ || reload_ || !queue_.empty() ||
           (timer_ && std::chrono::steady_clock::now() >= *timer_);
  };
  while (!ready()) {
    if (timer_) {
      wake_.wait_until(lock, *timer_);
    } else {
      wake_.wait(lock);
    }
  }
  fired.stop = stop_;
  fired.reload = std::exchange(reload_, false);
  if (timer_ && std::chrono::steady_clock::now() >= *timer_) {
    fired.timer = true;
    timer_.reset();
  }
  fired.changes.assign(queue_.begin(), queue_.end());
  queue_.clear();
  return true;
}

void DirEvents::OnEvents(size_t count, char** paths, const std::uint32_t* flags,
                         const std::uint64_t* ids) {
  const std::scoped_lock lock(mutex_);
  for (size_t i = 0; i < count; ++i) {
    since_ = ids[i];
    const std::uint32_t flag = flags[i];
    if (flag & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped |
                kFSEventStreamEventFlagKernelDropped)) {
      queue_.push_back({.wd = -1, .name = {}, .kind = Kind::kOverflow});
      continue;
    }
    const fs::path path(paths[i]);
    struct stat st{};
    const bool exists = ::lstat(paths[i], &st) == 0;
    const std::string parent = path.parent_path().string();
    for (const auto& [wd, watched] : dirs_) {
      if (watched.path == path.string() && (flag & kFSEventStreamEventFlagRootChanged) && !exists) {
        queue_.push_back({.wd = wd, .name = {}, .kind = Kind::kGone});
      } else if (watched.path == parent) {
        // One event can carry several flags, so whether the entry is there now decides.
        Kind kind;
        if (exists &&
            (flag & (kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemRenamed))) {
          kind = Kind::kAdded;
        } else if (exists && watched.writes && (flag & kFSEventStreamEventFlagItemModified)) {
          kind = Kind::kWritten;
        } else if (!exists && (flag & kFSEventStreamEventFlagItemRenamed)) {
          kind = Kind::kMovedAway;
        } else if (!exists && (flag & kFSEventStreamEventFlagItemRemoved)) {
          kind = Kind::kRemoved;
        } else {
          continue;
        }
        queue_.push_back({.wd = wd, .name = path.filename().string(), .kind = kind});
      }
    }
  }
  if (!queue_.empty()) wake_.notify_all();
}

}  // namespace mira::library

#endif
