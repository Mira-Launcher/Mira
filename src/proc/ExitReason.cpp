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
constexpr std::array kCrashSignals = {SIGSEGV, SIGABRT, SIGBUS, SIGILL,
                                      SIGFPE,  SIGTRAP, SIGSYS, SIGKILL};

std::string SignalReason(int signal_number) {
  switch (signal_number) {
    case SIGSEGV:
      return "invalid memory access (segmentation fault)";
    case SIGABRT:
      return "the game aborted itself (SIGABRT)";
    case SIGBUS:
      return "bus error (SIGBUS)";
    case SIGILL:
      return "illegal instruction, the CPU may lack a feature the game needs (SIGILL)";
    case SIGFPE:
      return "arithmetic error (SIGFPE)";
    case SIGTRAP:
      return "stopped at a breakpoint or trap (SIGTRAP)";
    case SIGSYS:
      return "bad system call (SIGSYS)";
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
  return text.substr(at + marker.size(), end == std::string_view::npos ? std::string_view::npos
                                                                       : end - at - marker.size());
}

// What Wine says went unhandled, e.g. "page fault on read access" or "C++
// exception", from its "wine: Unhandled ..." line; nullopt when there's none.
std::optional<std::string> WineUnhandled(std::string_view log) {
  std::string_view what = LastLineWith(log, "wine: Unhandled ");
  if (what.empty()) what = LastLineWith(log, "Unhandled exception code ");
  if (what.empty()) return std::nullopt;
  for (std::string_view cut : {" at address", " in thread", " (thread", ", starting", " flags "}) {
    if (const std::size_t at = what.find(cut); at != std::string_view::npos)
      what = what.substr(0, at);
  }
  // "page fault on read access to 0000000000000000": the address says nothing to a player.
  if (const std::size_t at = what.find(" to 0"); at != std::string_view::npos)
    what = what.substr(0, at);

  const std::string code =
      strings::ToLower(what.starts_with("exception 0x") ? what.substr(12) : what);
  static constexpr std::pair<std::string_view, std::string_view> kKnown[] = {
      {"c0000005", "access violation"}, {"c0000409", "stack buffer overrun"},
      {"c00000fd", "stack overflow"},   {"c0000374", "heap corruption"},
      {"e06d7363", "C++ exception"},    {"80000003", "breakpoint"},
  };
  for (const auto& [hex, name] : kKnown) {
    if (code.starts_with(hex)) return std::string(name);
  }
  return std::string(what);
}

// The DLL in Wine's "Library X.dll (which is needed by ...) not found" line, lowercased.
std::string MissingDll(std::string_view log) {
  constexpr std::string_view kMarker = "err:module:import_dll Library ";
  const std::size_t at = log.rfind(kMarker);
  if (at == std::string_view::npos) return {};
  const std::string_view rest = log.substr(at + kMarker.size());
  const std::string_view name = rest.substr(0, rest.find_first_of(" \n"));
  const std::size_t line_end = rest.find('\n');
  if (rest.substr(0, line_end).find("not found") == std::string_view::npos) return {};
  return strings::ToLower(name);
}

// The winetricks verb that installs `dll`; empty when Mira doesn't know one.
std::string VerbFor(std::string_view dll) {
  static constexpr std::pair<std::string_view, std::string_view> kPrefixes[] = {
      {"msvcp140", "vcrun2022"},
      {"vcruntime140", "vcrun2022"},
      {"concrt140", "vcrun2022"},
      {"vcomp140", "vcrun2022"},
      {"msvcr120", "vcrun2013"},
      {"msvcp120", "vcrun2013"},
      {"msvcr110", "vcrun2012"},
      {"msvcp110", "vcrun2012"},
      {"msvcr100", "vcrun2010"},
      {"msvcp100", "vcrun2010"},
      {"msvcr90", "vcrun2008"},
      {"msvcp90", "vcrun2008"},
      {"d3dx9_", "d3dx9"},
      {"d3dx10_", "d3dx10"},
      {"d3dx11_", "d3dx11_43"},
      {"d3dcompiler_43", "d3dcompiler_43"},
      {"d3dcompiler_47", "d3dcompiler_47"},
      {"xinput1_", "xinput"},
      {"physxloader", "physx"},
      {"xaudio2_", "xact"},
      {"x3daudio", "xact"},
      {"xactengine", "xact"},
  };
  for (const auto& [prefix, verb] : kPrefixes) {
    if (dll.starts_with(prefix)) return std::string(verb);
  }
  return {};
}

// Whether one line of `log` has both words, case ignored.
bool LineWithBoth(std::string_view log, std::string_view a, std::string_view b) {
  const std::string lower = strings::ToLower(log);
  for (std::size_t at = lower.find(a); at != std::string::npos; at = lower.find(a, at + 1)) {
    const std::size_t start = lower.rfind('\n', at);
    const std::size_t end = lower.find('\n', at);
    const std::string_view line = std::string_view(lower).substr(
        start == std::string::npos ? 0 : start + 1,
        end == std::string::npos ? std::string::npos
                                 : end - (start == std::string::npos ? 0 : start + 1));
    if (line.find(b) != std::string_view::npos) return true;
  }
  return false;
}

}  // namespace

std::optional<LogDiagnosis> DiagnoseLog(std::string_view log) {
  if (const std::string dll = MissingDll(log); !dll.empty()) {
    if (dll == "mscoree.dll") {
      return LogDiagnosis{.cause = "it needs .NET, which isn't in its prefix",
                          .hint = "Try a different Proton build, or install .NET with winetricks.",
                          .verb = {}};
    }
    const std::string verb = VerbFor(dll);
    return LogDiagnosis{
        .cause = std::format("it needs {}, which isn't in its prefix", dll),
        .hint = verb.empty()
                    ? "Installing the program that provides it in the game's prefix may help."
                    : std::format("Installing {} with winetricks usually fixes this.", verb),
        .verb = verb};
  }
  if (LineWithBoth(log, "vkcreateinstance", "fail") || LineWithBoth(log, "vulkan", "no suitable") ||
      LineWithBoth(log, "vulkan", "not supported")) {
    return LogDiagnosis{.cause = "Vulkan couldn't start",
                        .hint = "Update the graphics driver, including its 32-bit Vulkan driver.",
                        .verb = {}};
  }
  for (const char* anticheat : {"easyanticheat", "battleye"}) {
    if (LineWithBoth(log, anticheat, "error") || LineWithBoth(log, anticheat, "fail")) {
      return LogDiagnosis{.cause = "its anti-cheat stopped it",
                          .hint = "Its anti-cheat may not allow Linux; its details say.",
                          .verb = {}};
    }
  }
  return std::nullopt;
}

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

namespace {

ExitOutcome ClassifyByCode(const ExitInfo& info) {
  // Stop() sends SIGTERM, then SIGKILL, and a game may answer either with any exit code.
  if (info.requested_stop) return {};

  const auto start_failed = [](std::string why) {
    return ExitOutcome{
        .crashed = true, .code = "start_failed", .error = "Couldn't start: " + std::move(why)};
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
    return {
        .crashed = true,
        .code = "killed",
        .error = std::format("Killed after {}, often because the system ran out of memory", after)};
  }
  // Closed by the desktop, a terminal or another program: an ordinary end.
  if (signal_number == SIGTERM || signal_number == SIGINT || signal_number == SIGHUP) return {};
  if (signal_number != 0) {
    return {.crashed = true,
            .code = "crashed",
            .error = std::format("Crashed after {}: {}", after, SignalReason(signal_number))};
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

}  // namespace

ExitOutcome ClassifyExit(const ExitInfo& info) {
  ExitOutcome outcome = ClassifyByCode(info);
  if (info.requested_stop || outcome.code == "killed") return outcome;
  if (!outcome.crashed && info.exit_code <= 0) return outcome;
  const std::optional<LogDiagnosis> found = DiagnoseLog(info.log_tail);
  if (!found) return outcome;
  if (!outcome.crashed) {
    outcome = {.crashed = true,
               .code = "crashed",
               .error = std::format("Quit after {}: {}", DescribeDuration(info.played_seconds),
                                    found->cause),
               .hint = {},
               .verb = {}};
  }
  outcome.hint = found->hint;
  outcome.verb = found->verb;
  return outcome;
}

std::string ReadLogTail(const std::filesystem::path& log, std::size_t max_bytes) {
  std::ifstream file(log, std::ios::binary | std::ios::ate);
  if (!file) return {};
  const std::streamoff size = file.tellg();
  if (size < 0) return {};
  const std::streamoff start = size > static_cast<std::streamoff>(max_bytes)
                                   ? size - static_cast<std::streamoff>(max_bytes)
                                   : 0;
  file.seekg(start);
  std::string tail(static_cast<std::size_t>(size - start), '\0');
  file.read(tail.data(), static_cast<std::streamsize>(tail.size()));
  tail.resize(static_cast<std::size_t>(file.gcount()));
  return tail;
}

}  // namespace mira::proc
