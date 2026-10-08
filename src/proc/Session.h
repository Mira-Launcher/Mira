#pragma once

#include <sys/types.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/Result.h"

namespace mira::proc {

// One launch, start to finish. Written twice by mira-run: once right after
// the game is forked (game_pid set, finished false), once after it exits
// (finished true, everything below set). It is read by mirad both to serve
// GET /v1/games/{id} while it's running and, on startup, to reconcile a
// session that outlived a mirad restart (see docs/architecture.md).
struct SessionRecord {
  std::string game_id;
  pid_t wrapper_pid = 0;  // mira-run's own pid, used to detect a live session on restart
  pid_t game_pid = 0;     // the actual game; kept even if wrapper_pid dies, so it can still be Stop()ed
  std::int64_t started_at = 0;

  bool finished = false;
  std::int64_t ended_at = 0;
  std::int64_t duration_seconds = 0;  // from a monotonic clock, immune to a wall-clock jump mid-session
  int exit_code = -1;
  int signal = 0;
  std::string launch_error;  // set if execvp of the game itself failed

  std::optional<int> post_exit_code;
  bool post_timed_out = false;

  // Set by mirad's startup reconciliation for a session whose wrapper_pid
  // was already dead: the record is as complete as it can be made, but the
  // true end time/exit status were never observed.
  bool incomplete = false;
};

// Where mira-run writes a game's output; `state_dir` is the game store's folder.
std::filesystem::path GameLogPath(const std::filesystem::path& state_dir, const std::string& game_id);

// Session records live in mira.db's sessions table, keyed by game and start time. mira-run writes one as
// the session starts and again as it ends, through a connection of its own, so a record survives mirad
// dying; mirad counts it, which marks it counted. Writing never unmarks a counted record.
Result<void> WriteSessionRecord(const std::filesystem::path& database, const SessionRecord& record);
Result<SessionRecord> ReadSessionRecord(const std::filesystem::path& database, const std::string& game_id,
                                        std::int64_t started_at);
// Records mirad hasn't counted yet: sessions still running, or finished while it was down.
std::vector<SessionRecord> UncountedSessions(const std::filesystem::path& database);

// One-time move of sessions/*.toml (Mira 0.13 and earlier) into the database. Temporary.
void ImportSessionFiles(const std::filesystem::path& sessions_dir, const std::filesystem::path& database);

}  // namespace mira::proc
