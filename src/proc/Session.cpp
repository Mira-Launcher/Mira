#include "proc/Session.h"

#include <format>
#include <fstream>
#include <sstream>

#include <toml.hpp>

#include "core/AtomicFile.h"
#include "core/TomlJson.h"

namespace mira::proc {
namespace {
using nlohmann::json;

json ToJson(const SessionRecord& record) {
  json j = json::object();
  j["game_id"] = record.game_id;
  j["wrapper_pid"] = static_cast<std::int64_t>(record.wrapper_pid);
  j["game_pid"] = static_cast<std::int64_t>(record.game_pid);
  j["started_at"] = record.started_at;
  j["finished"] = record.finished;
  if (record.finished) {
    j["ended_at"] = record.ended_at;
    j["duration_seconds"] = record.duration_seconds;
    j["exit_code"] = record.exit_code;
    j["signal"] = record.signal;
    if (!record.launch_error.empty()) j["launch_error"] = record.launch_error;
    if (record.post_exit_code) j["post_exit_code"] = *record.post_exit_code;
    j["post_timed_out"] = record.post_timed_out;
  }
  j["incomplete"] = record.incomplete;
  return j;
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

}  // namespace

std::filesystem::path SessionFilePath(const std::filesystem::path& sessions_dir, const std::string& game_id,
                                      std::int64_t started_at) {
  return sessions_dir / std::format("{}-{}.toml", game_id, started_at);
}

std::filesystem::path GameLogPath(const std::filesystem::path& state_dir, const std::string& game_id) {
  return state_dir / "logs" / std::format("{}.log", game_id);
}

Result<void> WriteSessionRecord(const std::filesystem::path& path, const SessionRecord& record) {
  std::ostringstream text;
  text << tomljson::ToToml(ToJson(record));
  // Durable: a session record exists to survive a crash moments later.
  return WriteFileAtomic(path, text.str(), "session_write_failed", /*durable=*/true);
}

Result<SessionRecord> ReadSessionRecord(const std::filesystem::path& path) {
  toml::parse_result parsed = toml::parse_file(path.string());
  if (!parsed) return Err("session_read_failed", std::string(parsed.error().description()));
  return FromJson(tomljson::ToJson(parsed.table()));
}

}  // namespace mira::proc
