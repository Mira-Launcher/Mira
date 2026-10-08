#include "proc/Session.h"

#include <format>

#include "store/Database.h"

namespace mira::proc {
namespace {
using nlohmann::json;


}  // namespace

std::filesystem::path GameLogPath(const std::filesystem::path& state_dir, const std::string& game_id) {
  return state_dir / "logs" / std::format("{}.log", game_id);
}

namespace {
constexpr std::string_view kColumns =
    "game_id, wrapper_pid, game_pid, started_at, finished, ended_at, duration_seconds, exit_code, signal, "
    "launch_error, post_exit_code, post_timed_out, incomplete";

SessionRecord RowToRecord(const store::Statement& row) {
  SessionRecord record;
  record.game_id = row.Text(0);
  record.wrapper_pid = static_cast<pid_t>(row.Int(1));
  record.game_pid = static_cast<pid_t>(row.Int(2));
  record.started_at = row.Int(3);
  record.finished = row.Int(4) != 0;
  record.ended_at = row.Int(5);
  record.duration_seconds = row.Int(6);
  record.exit_code = static_cast<int>(row.Int(7));
  record.signal = static_cast<int>(row.Int(8));
  record.launch_error = row.Text(9);
  if (!row.IsNull(10)) record.post_exit_code = static_cast<int>(row.Int(10));
  record.post_timed_out = row.Int(11) != 0;
  record.incomplete = row.Int(12) != 0;
  return record;
}
}  // namespace

Result<void> WriteSessionRecord(const std::filesystem::path& database, const SessionRecord& record) {
  store::Database db;
  if (auto opened = db.Open(database); !opened) return opened;
  auto upsert = db.Prepare(std::format(
      "INSERT INTO sessions({}, counted) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0) "
      "ON CONFLICT(game_id, started_at) DO UPDATE SET wrapper_pid = excluded.wrapper_pid, "
      "game_pid = excluded.game_pid, finished = excluded.finished, ended_at = excluded.ended_at, "
      "duration_seconds = excluded.duration_seconds, exit_code = excluded.exit_code, signal = excluded.signal, "
      "launch_error = excluded.launch_error, post_exit_code = excluded.post_exit_code, "
      "post_timed_out = excluded.post_timed_out, incomplete = excluded.incomplete",
      kColumns));
  if (!upsert) return std::unexpected(upsert.error());
  upsert->Bind(1, record.game_id)
      .Bind(2, std::int64_t{record.wrapper_pid})
      .Bind(3, std::int64_t{record.game_pid})
      .Bind(4, record.started_at)
      .Bind(5, std::int64_t{record.finished})
      .Bind(6, record.ended_at)
      .Bind(7, record.duration_seconds)
      .Bind(8, std::int64_t{record.exit_code})
      .Bind(9, std::int64_t{record.signal})
      .Bind(10, record.launch_error)
      .Bind(12, std::int64_t{record.post_timed_out})
      .Bind(13, std::int64_t{record.incomplete});
  if (record.post_exit_code) {
    upsert->Bind(11, std::int64_t{*record.post_exit_code});
  } else {
    upsert->BindNull(11);
  }
  return upsert->Run();
}

Result<SessionRecord> ReadSessionRecord(const std::filesystem::path& database, const std::string& game_id,
                                        std::int64_t started_at) {
  store::Database db;
  if (auto opened = db.Open(database); !opened) return std::unexpected(opened.error());
  auto select = db.Prepare(std::format("SELECT {} FROM sessions WHERE game_id = ? AND started_at = ?", kColumns));
  if (!select) return std::unexpected(select.error());
  select->Bind(1, game_id).Bind(2, started_at);
  auto row = select->Step();
  if (!row) return std::unexpected(row.error());
  if (!*row) return Err("session_not_found", std::format("no session record for {} at {}", game_id, started_at));
  return RowToRecord(*select);
}

std::vector<SessionRecord> UncountedSessions(const std::filesystem::path& database) {
  std::vector<SessionRecord> records;
  store::Database db;
  if (!db.Open(database)) return records;
  auto select = db.Prepare(std::format("SELECT {} FROM sessions WHERE counted = 0 ORDER BY started_at", kColumns));
  if (!select) return records;
  for (auto row = select->Step(); row && *row; row = select->Step()) records.push_back(RowToRecord(*select));
  return records;
}


}  // namespace mira::proc
