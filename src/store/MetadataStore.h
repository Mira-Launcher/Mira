#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include <json.hpp>

#include "core/Result.h"
#include "store/Database.h"

namespace mira::store {

// Fetched store info and pointers to downloaded art, in cache.db beside the
// library. Images stay files under artwork/<id>/; a row says which file is a
// slot's and its version. Kept for any id, tracked game or store title. All of
// it can be fetched again, so a damaged cache.db is replaced by an empty one.
class MetadataStore {
public:
  explicit MetadataStore(std::filesystem::path dir);

  // Opens cache.db.
  void Load();

  // The id's info, an empty object when none is cached.
  nlohmann::json Read(const std::string& id) const;
  bool Has(const std::string& id) const;
  // Every cached id's top-level `key` from its info, null where it has none, in one query: for a
  // listing over the whole library without parsing each info whole.
  std::unordered_map<std::string, nlohmann::json> Field(std::string_view key) const;

  // Something fetched for the whole library rather than one id (Steam's tag names), by name; null
  // when none is cached.
  nlohmann::json ReadList(const std::string& name) const;
  Result<void> WriteList(const std::string& name, const nlohmann::json& value);
  // Replaces the id's info and its art rows together.
  Result<void> Write(const std::string& id, const nlohmann::json& info);
  // Drops the id's info, art rows and art files.
  void Remove(const std::string& id);

  std::filesystem::path ArtworkDir(const std::string& id) const;

  struct Art {
    std::filesystem::path file;
    std::string content_type;
  };
  // A slot ("cover", "hero", "logo", "icon") whose file is on disk.
  std::optional<Art> ArtFor(const std::string& id, std::string_view slot) const;
  // {"cover": "<version>", ...} for the slots with art, from memory: what a
  // library listing sends so a client refetches only changed images.
  nlohmann::json ArtVersions(const std::string& id) const;

private:
  Result<void> Open();
  Result<void> WriteLocked(const std::string& id, const nlohmann::json& info);

  std::filesystem::path dir_;
  mutable std::mutex mutex_;
  mutable Database db_;
  std::unordered_map<std::string, nlohmann::json> versions_;  // guarded by mutex_
};

}  // namespace mira::store
