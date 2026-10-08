#include "migrate/Legacy.h"

#include <filesystem>
#include <fstream>

#include <toml.hpp>

#include "config/Config.h"
#include "core/AtomicFile.h"
#include "core/Log.h"
#include "core/TomlJson.h"
#include "proc/Session.h"

namespace mira::migrate {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;
using proc::SessionRecord;

std::optional<json> ReadToml(const fs::path& file) {
  toml::parse_result parsed = toml::parse_file(file.string());
  if (!parsed) {
    log::Warn("could not read {}: {}", file.string(), parsed.error().description());
    return std::nullopt;
  }
  return tomljson::ToJson(parsed.table());
}

Result<SessionRecord> FromJson(const json& j) {
  if (!j.is_object() || !j.contains("game_id") || !j.contains("wrapper_pid") || !j.contains("started_at")) {
    return Err("session_record_invalid", "missing required fields");
  }
  SessionRecord record;
  record.game_id = j.value("game_id", "");
  record.wrapper_pid = static_cast<pid_t>(j.value("wrapper_pid", static_cast<std::int64_t>(0)));
  record.game_pid = static_cast<pid_t>(j.value("game_pid", static_cast<std::int64_t>(0)));
  record.started_at = j.value("started_at", static_cast<std::int64_t>(0));
  record.finished = j.value("finished", false);
  record.ended_at = j.value("ended_at", static_cast<std::int64_t>(0));
  record.duration_seconds = j.value("duration_seconds", static_cast<std::int64_t>(0));
  record.exit_code = j.value("exit_code", -1);
  record.signal = j.value("signal", 0);
  record.launch_error = j.value("launch_error", "");
  if (j.contains("post_exit_code") && j["post_exit_code"].is_number_integer()) {
    record.post_exit_code = j["post_exit_code"].get<int>();
  }
  record.post_timed_out = j.value("post_timed_out", false);
  record.incomplete = j.value("incomplete", false);
  return record;
}
void ImportGames(store::GameStore& games) {
  const fs::path file = games.Dir() / "games.toml";
  std::error_code ec;
  if (!fs::exists(file, ec)) return;
  const auto whole = ReadToml(file);
  if (!whole) {
    (void)SetAside(file);
    return;
  }
  int imported = 0;
  for (const json& entry : whole->value("game", json::array())) {
    model::Game game = model::GameFromJson(entry);
    if (game.id.empty()) continue;
    // Upsert is idempotent, so a failed import is simply tried again next start.
    if (auto written = games.Upsert(game); !written) {
      log::Error("could not import {}: {}", file.string(), written.error().message);
      return;
    }
    ++imported;
  }
  fs::rename(file, fs::path(file.string() + ".migrated"), ec);
  log::Info("imported {} game(s) from {}", imported, file.string());
}

void ImportMetadata(store::GameStore& games) {
  const fs::path dir = games.Dir() / "metadata";
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
  int imported = 0;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    std::ifstream in(entry.path());
    const json info = json::parse(in, nullptr, false);
    in.close();
    // A broken cache entry is fetched again, so it's dropped.
    if (info.is_object() && !games.Metadata().Write(entry.path().stem().string(), info)) continue;
    fs::remove(entry.path(), ec);
    ++imported;
  }
  fs::remove(dir, ec);  // only once empty
  log::Info("imported metadata for {} title(s)", imported);
}

void ImportSessions(store::GameStore& games) {
  const fs::path dir = games.Dir() / "sessions";
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    const auto whole = ReadToml(entry.path());
    const auto record = whole ? FromJson(*whole) : Err("session_read_failed", "unreadable");
    if (record && !proc::WriteSessionRecord(games.File(), *record)) continue;
    fs::remove(entry.path(), ec);
  }
  fs::remove(dir, ec);
}

void ImportUiState(store::GameStore& games) {
  const fs::path file = games.Dir() / "frontend.toml";
  std::error_code ec;
  if (!fs::exists(file, ec)) return;
  auto frontend = ReadToml(file);
  if (!frontend) return;
  json state = games.UiState();
  bool moved = false;
  for (const std::string_view key : config::kUiStateKeys) {
    if (!frontend->contains(key)) continue;
    state[std::string(key)] = (*frontend)[std::string(key)];
    frontend->erase(std::string(key));
    moved = true;
  }
  if (!moved) return;
  games.KeepUiState(state);
  (void)WriteFileAtomic(file, tomljson::ToTomlText(*frontend), "config_write_failed");
}
}  // namespace

void ImportLegacyFiles(store::GameStore& games) {
  ImportGames(games);
  ImportMetadata(games);
  ImportSessions(games);
  ImportUiState(games);
}

}  // namespace mira::migrate
