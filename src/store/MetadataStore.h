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

  // Opens cache.db; imports metadata/*.json once, then removes that folder.
  void Load();

  // The id's info, an empty object when none is cached.
  nlohmann::json Read(const std::string& id) const;
  bool Has(const std::string& id) const;
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
  void ImportFiles();

  std::filesystem::path dir_;
  mutable std::mutex mutex_;
  mutable Database db_;
  std::unordered_map<std::string, nlohmann::json> versions_;  // guarded by mutex_
};

}  // namespace mira::store
