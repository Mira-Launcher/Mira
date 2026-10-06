#pragma once

#include <sys/types.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

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

std::filesystem::path SessionFilePath(const std::filesystem::path& sessions_dir, const std::string& game_id,
                                      std::int64_t started_at);

// Where mira-run writes a game's output; `state_dir` is the game store's folder.
std::filesystem::path GameLogPath(const std::filesystem::path& state_dir, const std::string& game_id);

// Write-temp-then-rename, matching store::GameStore::Save's durability
// shape, plus an explicit fsync before the rename. A session record is
// exactly the kind of thing that must survive a crash a moment later, which
// is the whole reason it exists.
Result<void> WriteSessionRecord(const std::filesystem::path& path, const SessionRecord& record);

// A corrupt or truncated file is reported as an error, not thrown, so callers
// (mirad's reconciliation) are expected to log it, delete the file, and move
// on rather than fail startup over one bad record.
Result<SessionRecord> ReadSessionRecord(const std::filesystem::path& path);

}  // namespace mira::proc
