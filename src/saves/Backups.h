#pragma once

#include <cstdint>
#include <filesystem>
#include <json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"
#include "saves/Saves.h"

namespace mira::saves {

// Why a snapshot was taken. The first three are what saves.keep counts; the others are safety
// copies taken before Mira overwrites saves, kept while newer than the oldest of those.
inline constexpr std::string_view kFirst = "first";
inline constexpr std::string_view kAfterPlay = "after_play";
inline constexpr std::string_view kManual = "manual";
inline constexpr std::string_view kBeforeRestore = "before_restore";
inline constexpr std::string_view kBeforeCloudDownload = "before_cloud_download";

struct SnapshotFile {
  std::string root;  // a logical root (Roots)
  std::string path;  // inside it, '/'-separated
  std::uintmax_t bytes = 0;
  std::int64_t mtime_ns = 0;  // since the Unix epoch
};

struct Snapshot {
  std::string id;  // its folder's name: the UTC time it was taken, "-2" and on for a clash
  std::int64_t taken_at = 0;
  std::string reason;
  std::string entry{};
  std::vector<SnapshotFile> files{};
  std::uintmax_t bytes = 0;
  bool pinned = false;  // kept whatever saves.keep says, and not counted by it
};

// Where backups go: saves.backup_root, or "saves" in the first of mira_folders.
std::filesystem::path BackupRoot(const config::Config& config);
// One game's folder there; empty for an id that can't be a folder name.
std::filesystem::path GameBackups(const config::Config& config, std::string_view game_id);

// A game's snapshots in `dir` (GameBackups), newest first. Unreadable ones are left out.
std::vector<Snapshot> ListSnapshots(const std::filesystem::path& dir);
// {id, taken_at, reason, files, bytes, pinned}, as GET /v1/games/{id}/saves lists it.
nlohmann::json SnapshotJson(const Snapshot& snapshot);

// Copies the files `survey` found into a new snapshot of `game`, unless they are the newest
// snapshot's already (same paths, sizes and mtimes). {snapshot, taken}: the new snapshot, or the
// newest when nothing changed. Copies keep their mtimes; files unchanged since the newest snapshot
// are hard links to its copies. Written to <id>.partial and renamed when complete; stops early,
// leaving nothing, when ThisTaskStop() does. Then prunes old snapshots (saves.keep). Errs
// saves_not_found, saves_too_large (saves.max_mb), saves_disk_full, saves_failed or cancelled.
// The caller holds the game's claim, so nothing else writes its folder meanwhile.
Result<nlohmann::json> TakeSnapshot(const config::Config& config, const model::Game& game,
                                    const Survey& survey, std::string_view reason);

// One of a game's snapshots by id, or nullopt when there's none by that name.
std::optional<Snapshot> FindSnapshot(const std::filesystem::path& dir, std::string_view id);

// Copies `snapshot`'s files back where the game keeps them now, by their logical roots, with
// their mtimes. Each file is replaced in one step (a copy beside it, renamed over it); files the
// snapshot doesn't hold are left alone. Errs saves_unplaceable, before writing anything, when the
// game no longer has a folder the snapshot needs (a prefix for a game now native), and
// snapshot_not_found when it was deleted meanwhile. The caller holds the game's claim and took a
// before_restore snapshot first.
Result<void> RestoreSnapshot(const config::Config& config, const model::Game& game,
                             const Snapshot& snapshot);

// Pins one snapshot, so pruning never takes it, or unpins it. Errs snapshot_not_found.
Result<void> PinSnapshot(const config::Config& config, std::string_view game_id,
                         std::string_view id, bool pinned);

// Deletes one snapshot. Errs snapshot_not_found.
Result<void> DeleteSnapshot(const config::Config& config, std::string_view game_id,
                            std::string_view id);

// The disk space a game's backups take, counting each hard-linked copy once.
std::uintmax_t BackupBytes(const std::filesystem::path& dir);
// Deletes all of a game's backups, for a removal asked to (delete_saves).
Result<void> DeleteBackups(const config::Config& config, std::string_view game_id);

// The last after-play backup's failure, kept in the game's folder for the GUI until one succeeds.
void RecordFailure(const std::filesystem::path& dir, const std::optional<Error>& error);
// {code, message, hint?, at}, or null.
nlohmann::json LastFailure(const std::filesystem::path& dir);

}  // namespace mira::saves
