#include "proc/ExitReason.h"

#include <signal.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <fstream>
#include <optional>
#include <utility>

#include "core/Strings.h"

namespace mira::proc {
namespace {

// Signals a process only gets by crashing (or being force-killed), so a shell
// reporting 128 + one of them as its exit code is reporting a crashed child.
constexpr std::array kCrashSignals = {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS, SIGKILL};

std::string SignalReason(int signal_number) {
  switch (signal_number) {
    case SIGSEGV: return "invalid memory access (segmentation fault)";
    case SIGABRT: return "the game aborted itself (SIGABRT)";
    case SIGBUS: return "bus error (SIGBUS)";
    case SIGILL: return "illegal instruction, the CPU may lack a feature the game needs (SIGILL)";
    case SIGFPE: return "arithmetic error (SIGFPE)";
    case SIGTRAP: return "stopped at a breakpoint or trap (SIGTRAP)";
    case SIGSYS: return "bad system call (SIGSYS)";
    default: {
      const char* name = ::sigabbrev_np(signal_number);
      return name != nullptr ? std::format("signal {} (SIG{})", signal_number, name)
                             : std::format("signal {}", signal_number);
    }
  }
}

std::string_view LastLineWith(std::string_view text, std::string_view marker) {
  const std::size_t at = text.rfind(marker);
  if (at == std::string_view::npos) return {};
  const std::size_t end = text.find('\n', at);
  return text.substr(at + marker.size(), end == std::string_view::npos ? std::string_view::npos : end - at - marker.size());
}

// What Wine says went unhandled, e.g. "page fault on read access" or "C++
// exception", from its "wine: Unhandled ..." line; nullopt when there's none.
std::optional<std::string> WineUnhandled(std::string_view log) {
  std::string_view what = LastLineWith(log, "wine: Unhandled ");
  if (what.empty()) what = LastLineWith(log, "Unhandled exception code ");
  if (what.empty()) return std::nullopt;
  for (std::string_view cut : {" at address", " in thread", " (thread", ", starting", " flags "}) {
    if (const std::size_t at = what.find(cut); at != std::string_view::npos) what = what.substr(0, at);
  }
  // "page fault on read access to 0000000000000000": the address says nothing to a player.
  if (const std::size_t at = what.find(" to 0"); at != std::string_view::npos) what = what.substr(0, at);

  const std::string code = strings::ToLower(what.starts_with("exception 0x") ? what.substr(12) : what);
  static constexpr std::pair<std::string_view, std::string_view> kKnown[] = {
      {"c0000005", "access violation"},   {"c0000409", "stack buffer overrun"},
      {"c00000fd", "stack overflow"},     {"c0000374", "heap corruption"},
      {"e06d7363", "C++ exception"},      {"80000003", "breakpoint"},
  };
  for (const auto& [hex, name] : kKnown) {
    if (code.starts_with(hex)) return std::string(name);
  }
  return std::string(what);
}

}  // namespace

std::string DescribeDuration(std::int64_t seconds) {
  const auto plural = [](std::int64_t n, std::string_view unit) {
    return std::format("{} {}{}", n, unit, n == 1 ? "" : "s");
  };
  if (seconds < 60) return plural(std::max<std::int64_t>(seconds, 0), "second");
  if (seconds < 3600) return plural(seconds / 60, "minute");
  const std::int64_t minutes = (seconds % 3600) / 60;
  return minutes == 0 ? plural(seconds / 3600, "hour")
                      : plural(seconds / 3600, "hour") + " " + plural(minutes, "minute");
}

ExitOutcome ClassifyExit(const ExitInfo& info) {
  // Stop() sends SIGTERM, then SIGKILL, and a game may answer either with any exit code.
  if (info.requested_stop) return {};

  const auto start_failed = [](std::string why) {
    return ExitOutcome{.crashed = true, .code = "start_failed", .error = "Couldn't start: " + std::move(why)};
  };
  if (info.exit_code == 127) return start_failed("the program or a library it needs is missing");
  if (info.exit_code == 126) return start_failed("the program isn't executable");
  if (!info.launch_error.empty()) return start_failed(info.launch_error);

  const std::string after = DescribeDuration(info.played_seconds);
  int signal_number = info.signal;
  if (signal_number == 0 && info.exit_code > 128) {
    const int reported = info.exit_code - 128;
    if (std::ranges::contains(kCrashSignals, reported)) signal_number = reported;
  }
  if (signal_number == SIGKILL) {
    return {.crashed = true,
            .code = "killed",
            .error = std::format("Killed after {}, often because the system ran out of memory", after)};
  }
  // Closed by the desktop, a terminal or another program: an ordinary end.
  if (signal_number == SIGTERM || signal_number == SIGINT || signal_number == SIGHUP) return {};
  if (signal_number != 0) {
    return {.crashed = true, .code = "crashed", .error = std::format("Crashed after {}: {}", after, SignalReason(signal_number))};
  }

  if (info.exit_code > 0) {
    if (!LastLineWith(info.log_tail, "wine: cannot find ").empty() ||
        !LastLineWith(info.log_tail, "wine: could not load ").empty()) {
      return start_failed("Wine couldn't find or load the program");
    }
    if (const auto unhandled = WineUnhandled(info.log_tail)) {
      return {.crashed = true,
              .code = "crashed",
              .error = std::format("Crashed after {} with an unhandled {}", after, *unhandled)};
    }
  }
  return {};
}

std::string ReadLogTail(const std::filesystem::path& log, std::size_t max_bytes) {
  std::ifstream file(log, std::ios::binary | std::ios::ate);
  if (!file) return {};
  const std::streamoff size = file.tellg();
  const std::streamoff start = size > static_cast<std::streamoff>(max_bytes) ? size - static_cast<std::streamoff>(max_bytes) : 0;
  file.seekg(start);
  std::string tail(static_cast<std::size_t>(size - start), '\0');
  file.read(tail.data(), static_cast<std::streamsize>(tail.size()));
  tail.resize(static_cast<std::size_t>(file.gcount()));
  return tail;
}

}  // namespace mira::proc
