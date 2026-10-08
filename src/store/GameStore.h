#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/Result.h"
#include "store/Database.h"
#include "store/MetadataStore.h"
#include "model/Types.h"

namespace mira::store {

// The library, in an SQLite database (mira.db). One in-memory copy of every
// game, guarded by one mutex, answers reads; every mutation is written to the
// database in a transaction first and reaches the copy only once it has.
// One finished play session, kept as the game's history.
struct PlaySession {
  std::string game_id;
  std::int64_t started_at = 0;
  std::int64_t ended_at = 0;
  std::int64_t duration_seconds = 0;
  int exit_code = -1;
  int signal = 0;
  bool incomplete = false;  // mirad restarted during it, so how it ended is unknown
};

class GameStore {
public:
  explicit GameStore(std::filesystem::path file);

  // The directory the database lives in, the natural base for sibling
  // state (logs/) so it follows wherever a caller
  // (including a test) points the store, rather than hardcoding paths::UserDir().
  std::filesystem::path Dir() const { return file_.parent_path(); }
  const std::filesystem::path& File() const { return file_; }

  // Never fails, like Config::Load. Opens (or creates) the database, checks it
  // and backs it up to <file>.bak; a damaged file is set aside and the backup
  // used. A games.toml beside it is imported once, then renamed
  // games.toml.migrated.
  void Load();

  // The text of the last settings.toml that loaded cleanly, empty if none:
  // what Config::Load falls back to when the file is broken.
  // Fetched info and art pointers, in cache.db beside the library; Load opens it too.
  MetadataStore& Metadata() { return metadata_; }

  // The GUI's window state (sizes, sort, last filter): what it sets as it's used, kept out of frontend.toml.
  nlohmann::json UiState();
  void KeepUiState(const nlohmann::json& state);

  std::string SettingsSnapshot();
  void KeepSettingsSnapshot(const std::string& toml);

  // Changes whenever the library does, so a caller can keep what it derived from it until then.
  std::uint64_t Revision() const { return revision_.load(); }

  std::vector<model::Game> All() const;
  std::optional<model::Game> Find(const std::string& id) const;
  std::optional<model::Game> FindByInstallPath(const std::string& install_path) const;
  // Whether a game's install_path is inside `dir` (a game whose program sits in a subfolder).
  bool HasInstallUnder(const std::string& dir) const;

  // Derives an id from the game's name, disambiguating against existing ids
  // ("celeste", "celeste-2", ...) so ids stay readable.
  std::string NextId(const std::string& name) const;

  // Inserts or replaces a game wholesale, then saves.
  Result<void> Upsert(model::Game game);

  // Upsert for an importer that read `base`, edited `game`, then spent a while
  // provisioning: only the fields it changed are applied to the stored record,
  // so a rename, tag, playtime or status change made meanwhile survives. On
  // success `game` holds the stored result.
  Result<void> Merge(const std::optional<model::Game>& base, model::Game& game);

  // Applies `mutator` to the stored game under lock, saves, and returns the
  // updated copy. The common path for partial updates (PATCH, status
  // transitions) so read-modify-write is never split across two lock
  // acquisitions.
  Result<model::Game> Update(const std::string& id, std::function<void(model::Game&)> mutator);

  // Update for many games with one save. `mutator` returns false to leave a
  // game untouched. Returns the changed games; unknown ids are skipped.
  Result<std::vector<model::Game>> UpdateMany(const std::vector<std::string>& ids,
                                              std::function<bool(model::Game&)> mutator);

  // Held while scanning a library root and while moving or deleting a game's
  // folders, so a scan never sees a folder mid-move and adds it as a new game.
  [[nodiscard]] std::unique_lock<std::mutex> LockFolders() { return std::unique_lock(folders_mutex_); }

  // Update that also adds `session` to the game's history, in one transaction. A
  // session already recorded (same game and start) is not added again.
  Result<model::Game> FinishSession(const PlaySession& session, std::function<void(model::Game&)> mutator);
  // A game's sessions, newest first, at most `limit`.
  std::vector<PlaySession> Sessions(const std::string& id, int limit) const;

  Result<void> Remove(const std::string& id);
  // Removes every known id with one save and returns those removed.
  Result<std::vector<std::string>> RemoveMany(const std::vector<std::string>& ids);


private:
  Result<void> Open();
  void ImportToml();
  Result<void> Write(const model::Game& game);
  Result<void> Delete(const std::string& id);
  // Runs `write` in a transaction of its own.
  Result<void> Transact(const std::function<Result<void>()>& write);
  void ReadAll();

  mutable std::mutex mutex_;
  std::mutex folders_mutex_;
  std::filesystem::path file_;
  mutable Database db_;  // mutable for reads; guarded by mutex_
  MetadataStore metadata_;
  std::vector<model::Game> games_;
  // The database couldn't be opened, or games.toml couldn't be imported: refuse
  // changes rather than lose the library. Guarded by mutex_.
  bool read_only_ = false;
  std::atomic<std::uint64_t> revision_{0};
};

}  // namespace mira::store
