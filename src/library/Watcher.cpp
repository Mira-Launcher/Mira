#include "library/Watcher.h"

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
  if (!dir_events_.Ok()) log::Error("Watcher: failed to set up: {}", std::strerror(errno));
}

void Watcher::Stop() {
  dir_events_.WakeStop();
}

void Watcher::ReloadRoots() {
  dir_events_.WakeReload();
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
  for (const auto& [wd, watched] : watches_) dir_events_.Remove(wd);
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
      const int wd = dir_events_.Add(dir);
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
    const int wd = dir_events_.Add(folder);
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
  ++roots_watched_;  // events queue from here, read once the loop runs
}

bool Watcher::IsSortingFolder(const fs::path& root, const fs::path& dir) const {
  return dir != root && ContainerOf(config_, root, dir) && !games_.FindByInstallPath(dir.string());
}

void Watcher::WatchFolder(const fs::path& root, const fs::path& dir) {
  // Adding a watch on a folder already watched returns the same descriptor.
  const int wd = dir_events_.Add(dir);
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
    dir_events_.Remove(wd);
    return true;
  });
}

void Watcher::RearmTimer() {
  // A fixed short poll interval while anything is pending, not an idle
  // wakeup (see the class comment): it only runs while a directory is
  // actively being watched for size stability, and disarms the instant
  // pending_ empties.
  constexpr int kPollMs = 500;
  dir_events_.ArmTimer(pending_.empty() ? std::nullopt : std::optional(kPollMs));
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
  if (!dir_events_.Ok()) return;
  WatchRoots();
  // Editors save by writing a new file and renaming it over, so the folder is watched, not the
  // file.
  settings_wd_ = dir_events_.Add(config_.File().parent_path(), /*writes=*/true);

  DirEvents::Fired fired;
  while (dir_events_.Wait(fired)) {
    // Changes first: they belong to the watches a reload replaces.
    if (!fired.changes.empty()) HandleChanges(fired.changes);
    if (fired.reload) WatchRoots();
    if (fired.timer) HandleDebounceTick();
    if (fired.stop) return;
  }
  log::Error("Watcher: waiting for changes failed: {}", std::strerror(errno));
}

void Watcher::HandleChanges(const std::vector<DirEvents::Change>& changes) {
  using Kind = DirEvents::Kind;
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

  for (const DirEvents::Change& change : changes) {
    if (change.wd == settings_wd_ &&
        (change.kind == Kind::kWritten || change.kind == Kind::kAdded) &&
        config_.File().filename() == change.name) {
      settings_saved = true;
    }

    if (change.kind == Kind::kOverflow) {  // events were dropped: rescan every root
      for (const fs::path& root : config_.GetPathArray("library_roots")) deleted_from.insert(root);
      continue;
    }
    const auto watched = watches_.find(change.wd);
    if (watched == watches_.end()) continue;
    if (change.kind == Kind::kGone) {  // the folder itself went away
      watches_.erase(watched);
      continue;
    }
    const bool added = change.kind == Kind::kAdded;
    const bool removed = change.kind == Kind::kRemoved || change.kind == Kind::kMovedAway;
    const fs::path root = watched->second.root;
    const fs::path path = watched->second.dir / change.name;
    if (watched->second.mira_folder) {
      if (added) {
        ScheduleDrop(root, path);
      } else if (removed) {
        pending_.erase(path.string());
        if (drops_ != nullptr) drops_->Forget(path);
      }
      continue;
    }
    if (const std::string& emulator = watched->second.emulator; !emulator.empty()) {
      if (change.name.starts_with('.')) continue;
      if (added) {
        ScheduleCheck(root, path, /*is_archive=*/false, emulator);
      } else if (removed) {
        pending_.erase(path.string());
        roms_deleted.insert(emulator);
      }
      continue;
    }

    if (added) {
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
    } else if (removed) {
      pending_.erase(path.string());  // no point finishing a debounce for a path that's gone
      // A watch follows its folder, not the path: one moved away is dropped, and watched again
      // by its new path if it moved to another sorting folder's place (kAdded above).
      if (change.kind == Kind::kMovedAway) Unwatch(path);
      deleted_from.insert(root);
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
