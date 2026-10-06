#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/Result.h"
#include "model/Types.h"

namespace mira::store {

// The games.toml-backed source of truth for the library. One in-memory
// vector guarded by one mutex, matching Config's approach: at the scale this
// targets (hundreds of games, a single user) that is simpler than anything
// else and no slower in practice. Every mutation is saved to disk before it
// returns, so games.toml is never more than one write behind memory.
class GameStore {
public:
  explicit GameStore(std::filesystem::path file);

  // The directory games.toml itself lives in, the natural base for sibling
  // state (sessions/, logs/) so it follows wherever a caller
  // (including a test) points the store, rather than hardcoding paths::UserDir().
  std::filesystem::path Dir() const { return file_.parent_path(); }

  // Same never-fails contract as Config::Load: an unparseable file is kept as
  // <file>.bad and the library starts empty rather than the daemon refusing
  // to start.
  void Load();
  Result<void> Save();

  std::vector<model::Game> All() const;
  std::optional<model::Game> Find(const std::string& id) const;
  std::optional<model::Game> FindByInstallPath(const std::string& install_path) const;
  // Whether a game's install_path is inside `dir` (a game whose program sits in a subfolder).
  bool HasInstallUnder(const std::string& dir) const;

  // Derives an id from the game's name, disambiguating against existing ids
  // ("celeste", "celeste-2", ...) so games.toml stays readable.
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

  Result<void> Remove(const std::string& id);
  // Removes every known id with one save and returns those removed.
  Result<std::vector<std::string>> RemoveMany(const std::vector<std::string>& ids);

  // While one of these lives, saves are held back and written once when the
  // last one is destroyed (a failure is logged). For an import that upserts
  // many games, which would otherwise rewrite games.toml once per game.
  class SaveBatch {
  public:
    explicit SaveBatch(GameStore& store);
    ~SaveBatch();
    SaveBatch(const SaveBatch&) = delete;
    SaveBatch& operator=(const SaveBatch&) = delete;

  private:
    GameStore& store_;
  };
  [[nodiscard]] SaveBatch BatchSaves() { return SaveBatch(*this); }

private:
  mutable std::mutex mutex_;
  // Held for a whole Save: concurrent saves share one temp file, and the last
  // one to finish must also be the one holding the newest games_.
  std::mutex save_mutex_;
  std::mutex folders_mutex_;
  std::filesystem::path file_;
  std::vector<model::Game> games_;
  int batch_depth_ = 0;       // guarded by mutex_
  bool batch_dirty_ = false;  // a save was held back; guarded by mutex_
};

}  // namespace mira::store
