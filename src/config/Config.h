#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <json.hpp>

#include "core/Result.h"

namespace mira::config {

// The on-disk settings.toml, merged with schema defaults so every declared
// key always resolves. In memory everything is JSON; only Load()/Save()
// speak TOML (via core/TomlJson). Unknown keys are preserved across writes.
//
// frontend.toml is a separate, opaque sibling file: the daemon validates
// settings.toml against the schema, but stores/returns frontend.toml
// verbatim so the frontend can evolve its own settings shape freely.
class Config {
public:
  explicit Config(std::filesystem::path file);

  // Never fails: a missing file is created from defaults and an unparseable
  // one is preserved as <file>.bad and replaced. Priority one is that the
  // daemon starts and works, so a broken config degrades rather than blocking.
  void Load();
  Result<void> Save();

  nlohmann::json Document() const;  // backend keys only
  nlohmann::json Get(std::string_view key) const;

  Result<void> Set(std::string_view key, const nlohmann::json& value);
  // Applies a partial document, validating every key before changing anything.
  Result<void> Patch(const nlohmann::json& patch);
  Result<void> Reset(std::string_view key);
  void ResetAll();

  // The opaque [frontend] table, read and written without validation.
  nlohmann::json FrontendSettings() const;
  void SetFrontendSettings(nlohmann::json settings);

  bool GetBool(std::string_view key) const;
  std::int64_t GetInt(std::string_view key) const;
  double GetDouble(std::string_view key) const;
  std::string GetString(std::string_view key) const;
  std::vector<std::string> GetStringArray(std::string_view key) const;

  // String value with "~" and $VAR expanded.
  std::filesystem::path GetPath(std::string_view key) const;
  std::vector<std::filesystem::path> GetPathArray(std::string_view key) const;

  const std::filesystem::path& File() const { return file_; }
  // Changes whenever a setting does, so a caller can keep what it derived from them until then.
  std::uint64_t Revision() const { return revision_.load(); }

private:
  nlohmann::json GetLocked(std::string_view key) const;
  Result<void> SaveFrontendFile();

  mutable std::mutex mutex_;
  std::filesystem::path file_;           // settings.toml: backend keys only
  std::filesystem::path frontend_file_;  // frontend.toml: opaque, the frontend's own
  nlohmann::json document_;
  nlohmann::json frontend_ = nlohmann::json::object();
  // An unparseable file couldn't be set aside: saving would overwrite the only copy.
  bool keep_file_ = false;
  bool keep_frontend_file_ = false;
  std::atomic<std::uint64_t> revision_{0};
};

}  // namespace mira::config
