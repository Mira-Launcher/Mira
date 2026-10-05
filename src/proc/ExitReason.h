#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace mira::proc {

// How a game's process ended, as far as Mira saw it.
struct ExitInfo {
  int exit_code = -1;  // -1 when it was killed by a signal or never observed
  int signal = 0;
  bool requested_stop = false;  // Mira's Stop() sent the signal
  std::string launch_error;     // mira-run couldn't start the program
  std::int64_t played_seconds = 0;
  std::string log_tail;  // the end of the session's log, where Wine reports a crash
};

struct ExitOutcome {
  bool crashed = false;
  std::string code;   // "start_failed", "killed" or "crashed"; empty for a normal end
  std::string error;  // a plain sentence for the game's last_error; empty for a normal end
};

// A crash is what the process itself reports: a crash signal, a shell's
// 128+signal exit for one, a missing program, or Wine's "Unhandled ..."
// report for a Windows game that then exited non-zero. A plain non-zero
// exit isn't one: plenty of games return one on a normal quit.
ExitOutcome ClassifyExit(const ExitInfo& info);

// "12 seconds", "4 minutes", "2 hours 5 minutes".
std::string DescribeDuration(std::int64_t seconds);

// The last `max_bytes` of `log`, or empty when it can't be read.
std::string ReadLogTail(const std::filesystem::path& log, std::size_t max_bytes = 64 * 1024);

}  // namespace mira::proc
