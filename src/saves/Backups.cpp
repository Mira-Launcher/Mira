#include "saves/Backups.h"

#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <mutex>
#include <set>

#include "config/Resolver.h"
#include "core/AtomicFile.h"
#include "core/Files.h"
#include "core/Json.h"
#include "core/Lane.h"
#include "core/Log.h"
#include "core/Paths.h"

namespace mira::saves {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;

constexpr const char* kSnapshotFile = "snapshot.json";
constexpr const char* kStateFile = "state.json";
constexpr std::string_view kPartial = ".partial";
// Left free beyond the copies, so a backup never fills the disk to the last byte.
constexpr std::uintmax_t kSpareBytes = 64ull << 20;

// Held while a game's backup folder is read and changed, since deleting doesn't claim the game: a
// delete can't take a backup being restored.
std::unique_lock<std::mutex> LockBackups(const fs::path& dir) {
  static std::mutex guard;
  static std::map<std::string, std::mutex> folders;
  const std::lock_guard lock(guard);
  return std::unique_lock(folders[dir.string()]);
}

bool Counted(std::string_view reason) {
  return reason == kFirst || reason == kAfterPlay || reason == kManual;
}

// A relative '/'-separated path that stays inside the folder it's joined to.
bool SafeRelative(std::string_view path) {
  if (path.empty() || path.front() == '/') return false;
  for (const fs::path& part : fs::path(path))
    if (part == ".." || part == ".") return false;
  return true;
}

std::string Megabytes(std::uintmax_t bytes) {
  return std::format("{:.1f} MB", static_cast<double>(bytes) / (1 << 20));
}

std::optional<Snapshot> ReadSnapshot(const fs::path& folder) {
  const auto text = files::ReadFile(folder / kSnapshotFile);
  if (!text) return std::nullopt;
  const json doc = json::parse(*text, nullptr, false);
  if (!doc.is_object()) return std::nullopt;
  Snapshot snapshot{.id = folder.filename().string(),
                    .taken_at = core::JsonInt(doc, "taken_at"),
                    .reason = core::JsonString(doc, "reason"),
                    .entry = core::JsonString(doc, "entry")};
  for (const json& file : core::JsonArray(doc, "files")) {
    SnapshotFile item{.root = core::JsonString(file, "root"),
                      .path = core::JsonString(file, "path"),
                      .bytes = static_cast<std::uintmax_t>(core::JsonInt(file, "bytes")),
                      .mtime_ns = core::JsonInt(file, "mtime")};
    if (item.root.empty() || !SafeRelative(item.path)) continue;
    snapshot.bytes += item.bytes;
    snapshot.files.push_back(std::move(item));
  }
  return snapshot;
}

std::string NewId(const fs::path& dir) {
  const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  const std::string base = std::format("{:%Y%m%dT%H%M%SZ}", now);
  std::string id = base;
  std::error_code ec;
  for (int n = 2; fs::exists(dir / id, ec) || fs::exists(dir / (id + std::string(kPartial)), ec);
       ++n)
    id = std::format("{}-{}", base, n);
  return id;
}

// What a partial snapshot left: a backup stopped by a crash or a cancel.
void ClearPartials(const fs::path& dir) {
  for (const fs::path& entry : paths::ListDir(dir)) {
    if (!entry.filename().string().ends_with(kPartial)) continue;
    std::error_code ec;
    fs::remove_all(entry, ec);
  }
}

// The newest saves.keep counted snapshots, plus the safety copies newer than the oldest of them.
void Prune(const fs::path& dir, const std::vector<Snapshot>& newest_first, std::int64_t keep) {
  std::int64_t counted = 0;
  std::size_t oldest_kept = 0;
  std::vector<const Snapshot*> doomed;
  for (std::size_t i = 0; i < newest_first.size(); ++i) {
    if (!Counted(newest_first[i].reason)) continue;
    if (++counted <= keep)
      oldest_kept = i;
    else
      doomed.push_back(&newest_first[i]);
  }
  if (doomed.empty()) return;
  for (std::size_t i = oldest_kept + 1; i < newest_first.size(); ++i) {
    if (!Counted(newest_first[i].reason)) doomed.push_back(&newest_first[i]);
  }
  for (const Snapshot* snapshot : doomed) {
    std::error_code ec;
    fs::remove_all(dir / snapshot->id, ec);
    if (ec) log::Warn("couldn't remove the old save snapshot {}: {}", snapshot->id, ec.message());
  }
}

using FileKey = std::pair<std::string, std::string>;

}  // namespace

fs::path BackupRoot(const config::Config& config) {
  if (fs::path root = config.GetPath("saves.backup_root"); !root.empty()) return root;
  const std::vector<fs::path> folders = config.GetPathArray("mira_folders");
  return folders.empty() ? paths::Home() / "Mira" / "saves" : folders.front() / "saves";
}

fs::path GameBackups(const config::Config& config, std::string_view game_id) {
  if (game_id.empty() || game_id == "." || game_id == ".." ||
      game_id.find('/') != std::string_view::npos)
    return {};
  return BackupRoot(config) / game_id;
}

std::vector<Snapshot> ListSnapshots(const fs::path& dir) {
  std::vector<Snapshot> snapshots;
  if (dir.empty()) return snapshots;
  for (const fs::path& entry : paths::ListDir(dir)) {
    if (entry.filename().string().ends_with(kPartial)) continue;
    if (auto snapshot = ReadSnapshot(entry)) snapshots.push_back(std::move(*snapshot));
  }
  std::ranges::sort(snapshots, [](const Snapshot& a, const Snapshot& b) {
    return std::tie(a.taken_at, a.id) > std::tie(b.taken_at, b.id);
  });
  return snapshots;
}

json SnapshotJson(const Snapshot& snapshot) {
  return {{"id", snapshot.id},
          {"taken_at", snapshot.taken_at},
          {"reason", snapshot.reason},
          {"files", snapshot.files.size()},
          {"bytes", snapshot.bytes}};
}

Result<json> TakeSnapshot(const config::Config& config, const model::Game& game,
                          const Survey& survey, std::string_view reason) {
  const fs::path dir = GameBackups(config, game.id);
  if (dir.empty()) return Err("saves_failed", "This game's id can't name a folder.");

  // One copy of each file, however many locations found it.
  std::map<FileKey, const SaveFile*> found;
  std::uintmax_t total = 0;
  const Location* biggest = nullptr;
  std::uintmax_t biggest_bytes = 0;
  for (const Location& location : survey.locations) {
    std::uintmax_t bytes = 0;
    for (const SaveFile& file : location.files) {
      if (!SafeRelative(file.path)) continue;
      if (found.emplace(FileKey{file.root, file.path}, &file).second) total += file.bytes;
      bytes += file.bytes;
    }
    if (bytes > biggest_bytes) biggest = &location, biggest_bytes = bytes;
  }
  if (found.empty())
    return Err("saves_not_found", std::format("No saves of {} were found to back up.", game.name),
               "Mira finds saves through the save locations list. Add the game's own save "
               "folder under More save locations if it keeps them somewhere else.",
               Fix::Game(game.id, "saves"));
  const auto max_mb = config::Resolver(config, game.overrides).GetInt("saves.max_mb");
  if (total > static_cast<std::uintmax_t>(max_mb) << 20)
    return Err("saves_too_large",
               std::format("{}'s saves are {}, more than the {} MB a backup may take. Most of it "
                           "is in {}.",
                           game.name, Megabytes(total), max_mb, biggest->path),
               "Raise the size limit for this game's backups if all of it is saves.",
               Fix::Game(game.id, "saves"));

  const auto lock = LockBackups(dir);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec)
    return Err("saves_failed", std::format("Couldn't make {}: {}", dir.string(), ec.message()),
               kDiskHint, Fix::Setting("saves.backup_root"));
  ClearPartials(dir);

  std::vector<Snapshot> snapshots = ListSnapshots(dir);
  std::map<FileKey, const SnapshotFile*> previous;
  if (!snapshots.empty())
    for (const SnapshotFile& file : snapshots.front().files)
      previous.emplace(FileKey{file.root, file.path}, &file);
  const auto same = [&](const SaveFile& file) {
    const auto it = previous.find({file.root, file.path});
    return it != previous.end() && it->second->bytes == file.bytes &&
           it->second->mtime_ns == file.mtime_ns;
  };

  std::uintmax_t changed_bytes = 0;
  bool changed = previous.size() != found.size();
  for (const auto& [key, file] : found) {
    if (same(*file)) continue;
    changed = true;
    changed_bytes += file->bytes;
  }
  if (!changed) return json{{"snapshot", SnapshotJson(snapshots.front())}, {"taken", false}};

  const fs::space_info space = fs::space(dir, ec);
  if (!ec && space.available < changed_bytes + kSpareBytes)
    return Err("saves_disk_full",
               std::format("There isn't room for {}'s saves ({}) in {}.", game.name,
                           Megabytes(changed_bytes), dir.parent_path().string()),
               "Free some space on that disk, or keep backups on another one.",
               Fix::Setting("saves.backup_root"));

  const std::string id = NewId(dir);
  const fs::path partial = dir / (id + std::string(kPartial));
  const fs::path previous_files =
      snapshots.empty() ? fs::path() : dir / snapshots.front().id / "files";
  const auto fail = [&](std::unexpected<Error> error) {
    std::error_code ignored;
    fs::remove_all(partial, ignored);
    return error;
  };

  Snapshot snapshot{.id = id,
                    .taken_at = model::NowSeconds(),
                    .reason = std::string(reason),
                    .entry = survey.match ? survey.match->entry.name : std::string()};
  for (const auto& [key, file] : found) {
    if (ThisTaskStop().stop_requested()) return fail(Err("cancelled", "Cancelled"));
    const fs::path target = partial / "files" / file->root / file->path;
    fs::create_directories(target.parent_path(), ec);
    if (ec)
      return fail(
          Err("saves_failed",
              std::format("Couldn't write {}: {}", target.parent_path().string(), ec.message()),
              kDiskHint));

    SnapshotFile copied{.root = file->root, .path = file->path};
    if (same(*file)) {
      fs::create_hard_link(previous_files / file->root / file->path, target, ec);
      if (!ec) {
        copied.bytes = file->bytes;
        copied.mtime_ns = file->mtime_ns;
      }
    }
    if (!same(*file) || ec) {
      // Read the size and time as copied: the survey's may be a moment old.
      const auto bytes = fs::file_size(file->absolute, ec);
      const auto written = ec ? fs::file_time_type() : fs::last_write_time(file->absolute, ec);
      if (ec) {
        log::Info("{}'s save {} went away before its backup", game.id, file->path);
        continue;
      }
      fs::copy_file(file->absolute, target, ec);
      if (!ec) fs::last_write_time(target, written, ec);
      if (ec)
        return fail(Err("saves_failed",
                        std::format("Couldn't copy {}: {}", file->absolute.string(), ec.message()),
                        kDiskHint));
      copied.bytes = bytes;
      copied.mtime_ns = ToUnixNs(written);
    }
    snapshot.bytes += copied.bytes;
    snapshot.files.push_back(std::move(copied));
  }

  json files = json::array();
  for (const SnapshotFile& file : snapshot.files)
    files.push_back({{"root", file.root},
                     {"path", file.path},
                     {"bytes", file.bytes},
                     {"mtime", file.mtime_ns}});
  const json doc = {
      {"taken_at", snapshot.taken_at}, {"reason", snapshot.reason}, {"game_id", game.id},
      {"game_name", game.name},        {"entry", snapshot.entry},   {"files", std::move(files)}};
  if (auto written = WriteFileAtomic(partial / kSnapshotFile, doc.dump(2), "saves_failed", true);
      !written)
    return fail(std::unexpected(written.error()));
  fs::rename(partial, dir / id, ec);
  if (ec)
    return fail(Err("saves_failed",
                    std::format("Couldn't finish the backup in {}: {}", dir.string(), ec.message()),
                    kDiskHint));

  snapshots.insert(snapshots.begin(), snapshot);
  Prune(dir, snapshots, config::Resolver(config, game.overrides).GetInt("saves.keep"));
  return json{{"snapshot", SnapshotJson(snapshot)}, {"taken", true}};
}

std::optional<Snapshot> FindSnapshot(const fs::path& dir, std::string_view id) {
  if (dir.empty() || id.empty() || id == "." || id == ".." ||
      id.find('/') != std::string_view::npos || id.ends_with(kPartial))
    return std::nullopt;
  return ReadSnapshot(dir / id);
}

Result<void> RestoreSnapshot(const config::Config& config, const model::Game& game,
                             const Snapshot& snapshot) {
  const fs::path dir = GameBackups(config, game.id);
  const fs::path source = dir / snapshot.id / "files";
  const auto lock = LockBackups(dir);
  if (!FindSnapshot(dir, snapshot.id)) return Err("snapshot_not_found", "no such backup");
  const Roots roots = RootsFor(config, game);
  for (const SnapshotFile& file : snapshot.files) {
    if (RootPath(roots, file.root).empty())
      return Err("saves_unplaceable",
                 std::format("{} no longer has the folder this backup's {} came from.", game.name,
                             file.path),
                 "A backup from a Windows prefix can only go back into a prefix.");
  }
  for (const SnapshotFile& file : snapshot.files) {
    const fs::path target = RootPath(roots, file.root) / file.path;
    const fs::path temp =
        target.parent_path() / ("." + target.filename().string() + ".mira-restore");
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (!ec)
      fs::copy_file(source / file.root / file.path, temp, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::last_write_time(temp, FromUnixNs(file.mtime_ns), ec);
    if (!ec) fs::rename(temp, target, ec);
    if (ec) {
      std::error_code ignored;
      fs::remove(temp, ignored);
      return Err("saves_failed",
                 std::format("Couldn't put back {}: {}", target.string(), ec.message()), kDiskHint);
    }
  }
  return {};
}

Result<void> DeleteSnapshot(const config::Config& config, std::string_view game_id,
                            std::string_view id) {
  const fs::path dir = GameBackups(config, game_id);
  const auto lock = LockBackups(dir);
  if (!FindSnapshot(dir, id)) return Err("snapshot_not_found", "no such backup");
  std::error_code ec;
  fs::remove_all(dir / id, ec);
  if (ec)
    return Err("saves_failed",
               std::format("Couldn't delete {}: {}", (dir / id).string(), ec.message()), kDiskHint);
  return {};
}

std::uintmax_t BackupBytes(const fs::path& dir) {
  std::uintmax_t bytes = 0;
  std::set<std::pair<dev_t, ino_t>> seen;
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(dir, ec)) return 0;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::end(it);
       it.increment(ec)) {
    struct stat info{};
    if (::lstat(it->path().c_str(), &info) != 0 || !S_ISREG(info.st_mode)) continue;
    if (info.st_nlink > 1 && !seen.emplace(info.st_dev, info.st_ino).second) continue;
    bytes += static_cast<std::uintmax_t>(info.st_size);
  }
  return bytes;
}

Result<void> DeleteBackups(const config::Config& config, std::string_view game_id) {
  const fs::path dir = GameBackups(config, game_id);
  const auto lock = LockBackups(dir);
  std::error_code ec;
  if (dir.empty() || !fs::exists(dir, ec)) return {};
  // Never anything outside the backup folder, however the id or a link points.
  if (!paths::IsWithin(dir, {BackupRoot(config)}) || fs::is_symlink(dir, ec))
    return Err(
        "path_outside_root",
        std::format("{} isn't inside the backup folder, so it wasn't deleted.", dir.string()));
  fs::remove_all(dir, ec);
  if (ec)
    return Err("saves_failed",
               std::format("Couldn't delete the backups in {}: {}", dir.string(), ec.message()),
               kDiskHint);
  return {};
}

void RecordFailure(const fs::path& dir, const std::optional<Error>& error) {
  if (dir.empty()) return;
  std::error_code ec;
  if (!error) {
    fs::remove(dir / kStateFile, ec);
    return;
  }
  json failure = {{"code", error->code}, {"message", error->message}, {"at", model::NowSeconds()}};
  if (!error->hint.empty()) failure["hint"] = error->hint;
  fs::create_directories(dir, ec);
  if (auto written =
          WriteFileAtomic(dir / kStateFile, json{{"last_error", failure}}.dump(2), "saves_failed");
      !written)
    log::Warn("couldn't keep the failed backup's error: {}", written.error().message);
}

json LastFailure(const fs::path& dir) {
  if (dir.empty()) return nullptr;
  const auto text = files::ReadFile(dir / kStateFile);
  if (!text) return nullptr;
  const json doc = json::parse(*text, nullptr, false);
  const json failure = core::JsonObject(doc, "last_error");
  return failure.empty() ? json(nullptr) : failure;
}

}  // namespace mira::saves
