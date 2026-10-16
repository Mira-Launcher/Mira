#include "library/Watcher.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <vector>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "emulators/Emulators.h"
#include "library/ArchiveExtractor.h"
#include "library/FolderTags.h"
#include "library/Scanner.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

constexpr int kWatchMask = IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE;

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Sum of file sizes under `dir`, used only to tell whether a directory is
// still being written to. Errors on individual entries (permission, a file
// vanishing mid-walk) are skipped rather than failing the whole check. This
// only has to be approximately right, not exact.
std::uintmax_t TotalSize(const fs::path& dir) {
  std::uintmax_t total = 0;
  std::error_code ec;
  if (fs::is_regular_file(dir, ec))
    return fs::file_size(dir, ec);  // a game in one file, an AppImage
  for (const auto& entry :
       fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
    std::error_code size_ec;
    if (entry.is_regular_file(size_ec)) total += entry.file_size(size_ec);
  }
  return total;
}

// An archive's size across all its volumes.
std::uintmax_t ArchiveSize(const fs::path& archive) {
  std::uintmax_t total = 0;
  for (const fs::path& volume : VolumesOf(archive)) {
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(volume, ec);
    if (!ec) total += size;
  }
  return total;
}

}  // namespace

Watcher::Watcher(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {
  // Created here, before Run() ever starts on another thread, specifically
  // because Stop() (called from whatever thread owns the Watcher object) must
  // never race Run()'s own setup: thread creation is a happens-before point
  // for anything written beforehand, so this is the one fd that cannot be
  // deferred to Run() like the other three. TSan caught this exact race
  // during development (see tests/watcher_test.cpp).
  stop_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (stop_fd_ < 0) log::Error("Watcher: failed to create stop eventfd: {}", std::strerror(errno));
  // Same reasoning as stop_fd_.
  reload_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (reload_fd_ < 0)
    log::Error("Watcher: failed to create reload eventfd: {}", std::strerror(errno));
}

Watcher::~Watcher() {
  if (inotify_fd_ >= 0) ::close(inotify_fd_);
  if (epoll_fd_ >= 0) ::close(epoll_fd_);
  if (timer_fd_ >= 0) ::close(timer_fd_);
  if (stop_fd_ >= 0) ::close(stop_fd_);
  if (reload_fd_ >= 0) ::close(reload_fd_);
}

void Watcher::Stop() {
  if (stop_fd_ < 0) return;
  const uint64_t one = 1;
  if (::write(stop_fd_, &one, sizeof(one)) < 0) {
    log::Error("Watcher::Stop: failed to signal eventfd: {}", std::strerror(errno));
  }
}

void Watcher::ReloadRoots() {
  if (reload_fd_ < 0) return;
  const uint64_t one = 1;
  if (::write(reload_fd_, &one, sizeof(one)) < 0) {
    log::Error("Watcher::ReloadRoots: failed to signal eventfd: {}", std::strerror(errno));
  }
}

void CreateMissingRoots(const config::Config& config) {
  // Resolved, so /home/you and /var/home/you (Fedora Atomic's symlink) count as the same home.
  std::error_code ec;
  const fs::path home = fs::weakly_canonical(paths::Home(), ec);
  const std::vector<fs::path> mira_folders = config.GetPathArray("mira_folders");
  for (const fs::path& root : config.GetPathArray("library_roots")) {
    const fs::path normal = root.lexically_normal();
    const fs::path relative = fs::weakly_canonical(normal, ec).lexically_relative(home);
    // One in a Mira folder on another disk is made too, while that disk is there.
    const bool in_mira_folder = std::ranges::any_of(mira_folders, [&](const fs::path& folder) {
      return IsAtOrUnder(normal, folder) && fs::is_directory(folder, ec);
    });
    if (!in_mira_folder && (relative.empty() || relative == "." || *relative.begin() == ".."))
      continue;
    if (fs::exists(normal, ec)) continue;
    if (fs::create_directories(normal, ec)) {
      log::Info("created library root {}", normal.string());
    } else if (ec) {
      log::Warn("could not create library root {}: {}", normal.string(), ec.message());
    }
  }
}

void Watcher::WatchRoots() {
  for (const auto& [wd, watched] : watches_) inotify_rm_watch(inotify_fd_, wd);
  watches_.clear();
  pending_.clear();
  RearmTimer();
  CreateMissingRoots(config_);

  for (const fs::path& root : config_.GetPathArray("library_roots")) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
      log::Warn("Watcher: library root {} does not exist yet, so it is not watched until it does",
                root.string());
      continue;
    }
    WatchFolder(root, root);
  }
  for (const emulators::Emulator& emulator : emulators::All()) {
    if (!games_.Source(emulator.id).added) continue;
    std::error_code ec;
    fs::create_directories(emulators::RomDir(config_, emulator), ec);
    for (const fs::path& dir : emulators::GameRoots(config_, emulator)) {
      const int wd = inotify_add_watch(inotify_fd_, dir.c_str(), kWatchMask);
      if (wd < 0) {
        log::Error("Watcher: could not watch {}: {}", dir.string(), std::strerror(errno));
        continue;
      }
      if (watches_.contains(wd))
        continue;  // already a library root's: its games aren't the emulator's alone
      watches_[wd] = {.root = dir, .dir = dir, .emulator = emulator.id};
    }
  }
  for (const fs::path& folder : config_.GetPathArray("mira_folders")) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) continue;
    const int wd = inotify_add_watch(inotify_fd_, folder.c_str(), kWatchMask);
    if (wd < 0) {
      log::Error("Watcher: could not watch {}: {}", folder.string(), std::strerror(errno));
      continue;
    }
    if (watches_.contains(wd)) continue;  // also a library root, scanned as one
    watches_[wd] = {.root = folder, .dir = folder, .mira_folder = true};
    // What was there before: asked about, since it may be the user's own.
    for (const fs::path& entry : paths::ListDir(folder)) {
      if (drops_ != nullptr && !IsMiraFoldersOwn(config_, entry)) drops_->Sort(entry, /*ask=*/true);
    }
  }
  ++roots_watched_;  // events queue in inotify from here, read once the loop runs
}

bool Watcher::IsSortingFolder(const fs::path& root, const fs::path& dir) const {
  return dir != root && ContainerOf(config_, root, dir) && !games_.FindByInstallPath(dir.string());
}

void Watcher::WatchFolder(const fs::path& root, const fs::path& dir) {
  // Adding a watch on a folder already watched returns the same descriptor.
  const int wd = inotify_add_watch(inotify_fd_, dir.c_str(), kWatchMask);
  if (wd < 0) {
    log::Error("Watcher: could not watch {}: {}", dir.string(), std::strerror(errno));
    return;
  }
  if (!watches_.contains(wd)) log::Info("watching {}", dir.string());
  watches_[wd] = {.root = root, .dir = dir};
  std::error_code ec;
  for (const fs::path& entry : paths::ListDir(dir)) {
    // Never through a link: Mira's own point into prefixes.
    if (fs::is_directory(fs::symlink_status(entry, ec)) && IsSortingFolder(root, entry))
      WatchFolder(root, entry);
  }
}

void Watcher::Unwatch(const fs::path& dir) {
  std::erase_if(watches_, [&](const auto& entry) {
    const auto& [wd, watched] = entry;
    if (watched.dir == watched.root || !IsAtOrUnder(watched.dir, dir)) return false;
    inotify_rm_watch(inotify_fd_, wd);
    return true;
  });
}

void Watcher::RearmTimer() {
  itimerspec spec{};
  if (pending_.empty()) {
    timerfd_settime(timer_fd_, 0, &spec, nullptr);  // all-zero disarms it
    return;
  }
  // A fixed short poll interval while anything is pending, not an idle
  // wakeup (see the class comment): it only runs while a directory is
  // actively being watched for size stability, and disarms the instant
  // pending_ empties.
  constexpr long kPollMs = 500;
  spec.it_value.tv_sec = kPollMs / 1000;
  spec.it_value.tv_nsec = (kPollMs % 1000) * 1000000L;
  timerfd_settime(timer_fd_, 0, &spec, nullptr);
}

void Watcher::ScheduleCheck(const fs::path& root, const fs::path& path, bool is_archive,
                            const std::string& emulator, bool drop) {
  Pending entry;
  entry.root = root;
  entry.is_archive = is_archive;
  entry.emulator = emulator;
  entry.drop = drop;
  entry.last_size = is_archive ? ArchiveSize(path) : TotalSize(path);
  entry.stable_since_ms = NowMs();
  pending_[path.string()] = entry;
  RearmTimer();
}

void Watcher::ScheduleDrop(const fs::path& folder, const fs::path& path) {
  if (drops_ == nullptr || IsMiraFoldersOwn(config_, path)) return;
  if (!config_.GetBool("scan.auto_extract_archives")) {
    ScheduleCheck(folder, path, /*is_archive=*/false, {}, /*drop=*/true);
  } else if (IsLaterVolume(path)) {
    if (auto first = FirstVolumeOf(path)) ScheduleCheck(folder, *first, true, {}, true);
  } else {
    ScheduleCheck(folder, path, LooksLikeArchive(path), {}, /*drop=*/true);
  }
}

void Watcher::Run() {
  inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (inotify_fd_ < 0 || epoll_fd_ < 0 || timer_fd_ < 0 || stop_fd_ < 0) {
    log::Error("Watcher: failed to set up inotify/epoll: {}", std::strerror(errno));
    return;
  }

  WatchRoots();
  // Editors save by writing a new file and renaming it over, so the folder is watched, not the
  // file.
  settings_wd_ = inotify_add_watch(inotify_fd_, config_.File().parent_path().c_str(),
                                   IN_CLOSE_WRITE | IN_MOVED_TO);

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = inotify_fd_;
  epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, inotify_fd_, &ev);
  ev.data.fd = timer_fd_;
  epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &ev);
  ev.data.fd = stop_fd_;
  epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, stop_fd_, &ev);
  ev.data.fd = reload_fd_;
  epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, reload_fd_, &ev);

  epoll_event fired[8];
  while (true) {
    const int n = epoll_wait(epoll_fd_, fired, 8, -1);  // blocks; no timeout at rest
    if (n < 0) {
      if (errno == EINTR) continue;
      log::Error("Watcher: epoll_wait failed: {}", std::strerror(errno));
      break;
    }
    bool stop_requested = false;
    for (int i = 0; i < n; ++i) {
      if (fired[i].data.fd == stop_fd_) {
        stop_requested = true;
      } else if (fired[i].data.fd == reload_fd_) {
        uint64_t signals = 0;
        [[maybe_unused]] auto _ = ::read(reload_fd_, &signals, sizeof(signals));
        WatchRoots();
      } else if (fired[i].data.fd == inotify_fd_) {
        HandleInotify();
      } else if (fired[i].data.fd == timer_fd_) {
        uint64_t expirations = 0;
        [[maybe_unused]] auto _ = ::read(timer_fd_, &expirations, sizeof(expirations));
        HandleDebounceTick();
      }
    }
    if (stop_requested) break;
  }
}

void Watcher::HandleInotify() {
  // Sized for several events with reasonably long filenames; inotify_event
  // is variable-length (name follows the struct), so this is read in a loop
  // rather than assumed to be one event per read.
  alignas(inotify_event) char buffer[4096];
  std::set<fs::path> deleted_from;     // roots to rescan once the queue is drained
  std::set<std::string> roms_deleted;  // emulators to import into once it is
  bool settings_saved = false;

  // Computed once per call, not per event: a runner build being downloaded
  // (runner/Downloader.cpp) into runner_search_paths/wine_search_paths must
  // never also be treated as a new game folder or a droppable archive here,
  // and a library_roots entry can overlap these.
  std::vector<fs::path> runner_roots;
  for (const fs::path& p : config_.GetPathArray("runner_search_paths")) runner_roots.push_back(p);
  for (const fs::path& p : config_.GetPathArray("wine_search_paths")) runner_roots.push_back(p);

  while (true) {
    const ssize_t n = ::read(inotify_fd_, buffer, sizeof(buffer));
    if (n <= 0) break;  // EAGAIN (nothing more queued) or an error either way

    size_t offset = 0;
    while (offset < static_cast<size_t>(n)) {
      const auto* event = reinterpret_cast<const inotify_event*>(buffer + offset);
      offset += sizeof(inotify_event) + event->len;
      if (event->wd == settings_wd_ && event->len > 0 && config_.File().filename() == event->name) {
        settings_saved = true;
      }

      if (event->mask & IN_Q_OVERFLOW) {  // events were dropped: rescan every root
        for (const fs::path& root : config_.GetPathArray("library_roots"))
          deleted_from.insert(root);
        continue;
      }
      const auto watched = watches_.find(event->wd);
      if (watched == watches_.end()) continue;
      if (event->mask & IN_IGNORED) {  // the folder itself went away
        watches_.erase(watched);
        continue;
      }
      if (event->len == 0) continue;
      const fs::path root = watched->second.root;
      const fs::path path = watched->second.dir / event->name;
      if (watched->second.mira_folder) {
        if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
          ScheduleDrop(root, path);
        } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
          pending_.erase(path.string());
          if (drops_ != nullptr) drops_->Forget(path);
        }
        continue;
      }
      if (const std::string& emulator = watched->second.emulator; !emulator.empty()) {
        if (std::string_view(event->name).starts_with('.')) continue;
        if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
          ScheduleCheck(root, path, /*is_archive=*/false, emulator);
        } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
          pending_.erase(path.string());
          roms_deleted.insert(emulator);
        }
        continue;
      }

      if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
        if (paths::IsWithin(path, runner_roots, /*allow_equal=*/true)) continue;
        std::error_code ec;
        const bool extract = config_.GetBool("scan.auto_extract_archives");
        // A link to a folder arrives whole (no copy to wait out, and its size is a whole game's).
        if (fs::is_symlink(fs::symlink_status(path, ec)) && fs::is_directory(path, ec)) {
          deleted_from.insert(root);  // rescanned right away, like a deletion
        } else if (fs::is_directory(path, ec)) {
          if (IsSortingFolder(root, path)) WatchFolder(root, path);
          if (!path.filename().string().starts_with(kExtractingPrefix)) {
            ScheduleCheck(root, path, /*is_archive=*/false);
          }
        } else if (strings::ToLower(path.extension().string()) == ".appimage") {
          ScheduleCheck(root, path, /*is_archive=*/false);  // a game in one file
        } else if (extract && LooksLikeArchive(path)) {
          ScheduleCheck(root, path, /*is_archive=*/true);
        } else if (extract && IsLaterVolume(path)) {
          // Another part arriving restarts the wait on the first one.
          if (auto first = FirstVolumeOf(path)) ScheduleCheck(root, *first, /*is_archive=*/true);
        }
      } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
        pending_.erase(path.string());  // no point finishing a debounce for a path that's gone
        // A watch follows its folder, not the path: one moved away is dropped, and watched again
        // by its new path if it moved to another sorting folder's place (IN_MOVED_TO above).
        if (event->mask & IN_MOVED_FROM) Unwatch(path);
        deleted_from.insert(root);
      }
    }
  }
  if (settings_saved && on_settings_saved_) on_settings_saved_();
  if (!roms_deleted.empty()) ImportRoms(roms_deleted);
  // A deletion needs no debounce: rescan now so a removed game is marked
  // missing promptly. Once per root, however many entries went at once.
  if (!deleted_from.empty()) {
    library::Scanner scanner(config_, games_, events_);
    scanner.UseMetadataQueue(*metadata_fetches_);
    if (installs_) scanner.UseInstallLane(*installs_);
    if (unclear_moves_) scanner.UseUnclearMoves(*unclear_moves_);
    for (const fs::path& root : deleted_from) {
      const ScanSummary summary = scanner.ScanRoot(root);
      for (const model::Game& game : summary.added_games)
        metadata_fetches_->EnqueueNew(config_, events_, game);
    }
  }
  RearmTimer();
}

void Watcher::HandleDebounceTick() {
  const std::int64_t now = NowMs();
  const std::int64_t debounce_ms = config_.GetInt("scan.debounce_ms");
  std::vector<std::string> settled;

  for (auto& [path, entry] : pending_) {
    // Walked once per debounce window, not every tick: a big tree's walk is slow.
    if (now - entry.stable_since_ms < debounce_ms) continue;
    std::error_code ec;
    const std::uintmax_t current_size = entry.is_archive ? ArchiveSize(path) : TotalSize(path);
    // A downloader that preallocates the file keeps its size fixed, so an
    // archive also waits until nothing has it open for writing.
    if (current_size != entry.last_size ||
        (entry.is_archive && AnyOpenForWriting(VolumesOf(path)))) {
      entry.last_size = current_size;
      entry.stable_since_ms = now;
      continue;
    }
    if (now - entry.stable_since_ms >= debounce_ms) settled.push_back(path);
  }

  if (!settled.empty()) {
    // Extractions run one by one; each root is scanned once afterwards, however many paths settled
    // in it.
    std::set<fs::path> to_scan;
    std::set<std::string> roms;
    for (const std::string& path : settled) {
      const Pending entry = pending_.at(path);
      pending_.erase(path);
      if (!entry.emulator.empty()) {
        roms.insert(entry.emulator);
        continue;
      }
      if (entry.drop && !entry.is_archive) {
        // Sorted into a library or emulator folder, whose own watch then picks it up.
        drops_->Sort(path);
        continue;
      }

      if (entry.is_archive) {
        const fs::path archive(path);
        const fs::path dest = entry.root / library::StemWithoutArchiveExtension(archive);
        log::Info("{} settled, extracting into {}", path, dest.string());
        if (auto extracted = library::ExtractAndRemove(archive, dest); !extracted) {
          log::Error("failed to extract {}: {}", path, extracted.error().message);
          continue;
        }
        // Extracted into a Mira folder: sorted when its folder appears there.
        if (entry.drop) continue;
      } else {
        log::Info("{} settled, scanning {}", path, entry.root.string());
      }
      to_scan.insert(entry.root);
    }
    library::Scanner scanner(config_, games_, events_);
    scanner.UseMetadataQueue(*metadata_fetches_);
    if (installs_) scanner.UseInstallLane(*installs_);
    if (unclear_moves_) scanner.UseUnclearMoves(*unclear_moves_);
    for (const fs::path& root : to_scan) {
      const ScanSummary summary = scanner.ScanRoot(root);
      for (const model::Game& game : summary.added_games)
        metadata_fetches_->EnqueueNew(config_, events_, game);
    }
    if (!roms.empty()) ImportRoms(roms);
  }
  RearmTimer();
}

void Watcher::ImportRoms(const std::set<std::string>& ids) {
  for (const std::string& id : ids) {
    const emulators::Emulator* emulator = emulators::Find(id);
    if (emulator == nullptr) continue;
    log::Info("{}'s games folder changed, importing", emulator->name);
    const auto summary = emulators::Import(config_, games_, events_, *emulator);
    if (!summary) continue;
    for (const model::Game& game : summary->added_games)
      metadata_fetches_->EnqueueNew(config_, events_, game);
  }
}

}  // namespace mira::library
