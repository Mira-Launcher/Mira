#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <json.hpp>

#include "core/Result.h"

namespace mira::config {

// Frontend keys the GUI sets as it's used, rather than a person choosing them: kept in mira.db, not frontend.toml.
inline constexpr std::array<std::string_view, 11> kUiStateKeys = {
    "window_width", "window_height",  "window_maximized", "sidebar_width", "details_width", "tile_width",
    "source_tile_widths", "library_filter", "sort_by", "sort_descending", "onboarded"};

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
  // one is preserved as <file>.bad and replaced, by `fallback` (the last file
  // that parsed) when there is one, otherwise by defaults. Priority one is that
  // the daemon starts and works, so a broken config degrades rather than blocking.
  void Load(const std::string& fallback = {});
  // Picks up a hand edit made since the file was last read or written: the keys
  // whose value changed. A file that doesn't parse changes nothing and is an error
  // naming the line; a bad value keeps the current one.
  Result<std::vector<std::string>> Reload();
  // Called with the text of each settings.toml that loads or is written cleanly.
  void OnValidText(std::function<void(const std::string&)> callback);
  // The text of settings.toml as Load parsed it, empty when it didn't.
  std::string LoadedText() const;

  nlohmann::json Document() const;  // backend keys only
  nlohmann::json Get(std::string_view key) const;

  Result<void> Set(std::string_view key, const nlohmann::json& value);
  // Applies a partial document, validating every key before changing anything.
  Result<void> Patch(const nlohmann::json& patch);
  Result<void> Reset(std::string_view key);
  Result<void> ResetAll();

  // Where the GUI's window state (kUiStateKeys) is kept instead of frontend.toml,
  // which holds only what a person would set. Before Load().
  void UseUiStateStore(std::function<nlohmann::json()> read, std::function<void(const nlohmann::json&)> write);

  // The opaque [frontend] table, read and written without validation: frontend.toml
  // with the window state merged in.
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
  Result<std::vector<std::string>> SyncLocked();
  // Every app change: picks up hand edits first, so only the keys the app set change.
  Result<void> ChangeAndSave(const std::function<void()>& change);
  Result<void> SaveFrontendFile();
  // Moves the window-state keys of `table` into ui_state_; whether any were there.
  bool TakeUiState(nlohmann::json& table);

  mutable std::mutex mutex_;
  std::filesystem::path file_;           // settings.toml: backend keys only
  std::filesystem::path frontend_file_;  // frontend.toml: opaque, the frontend's own
  nlohmann::json document_;
  nlohmann::json frontend_ = nlohmann::json::object();
  nlohmann::json ui_state_ = nlohmann::json::object();
  std::function<nlohmann::json()> read_ui_state_;
  std::function<void(const nlohmann::json&)> write_ui_state_;
  // An unparseable file couldn't be set aside: saving would overwrite the only copy.
  std::string loaded_text_;  // as last read or written, to tell a hand edit apart
  std::function<void(const std::string&)> on_valid_text_;
  bool keep_file_ = false;
  bool keep_frontend_file_ = false;
  std::atomic<std::uint64_t> revision_{0};
};

}  // namespace mira::config
