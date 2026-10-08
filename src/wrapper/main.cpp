// mira-run: the process that actually sits between mirad and a running
// game. Owns the whole session end to end: pre_script, the game itself,
// post_script, and the session record, so none of that depends on mirad
// staying alive for the session's whole duration. See docs/architecture.md
// and proc/Session.h.
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/Result.h"
#include "model/Types.h"
#include "proc/Session.h"
#include "runner/GameMode.h"

using namespace mira;

namespace {

struct Args {
  std::string game_id;
  std::filesystem::path database;  // mira.db, where the session record goes
  std::filesystem::path log_file;  // empty disables per-game logging entirely
  int log_max_mb = 64;
  int status_fd = -1;
  std::string pre;
  std::string post;
  int pre_timeout_s = 30;
  int post_timeout_s = 30;
  bool gamemode = false;
  std::vector<std::string> game_argv;
};

std::optional<Args> ParseArgs(int argc, char** argv) {
  Args args;
  int i = 1;
  for (; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto next = [&]() -> std::optional<std::string> {
      if (i + 1 >= argc) return std::nullopt;
      return std::string(argv[++i]);
    };
    if (a == "--game-id") {
      auto v = next();
      if (!v) return std::nullopt;
      args.game_id = *v;
    } else if (a == "--database") {
      auto v = next();
      if (!v) return std::nullopt;
      args.database = *v;
    } else if (a == "--log-file") {
      auto v = next();
      if (!v) return std::nullopt;
      args.log_file = *v;
    } else if (a == "--log-max-mb") {
      auto v = next();
      if (!v) return std::nullopt;
      args.log_max_mb = std::atoi(v->c_str());
    } else if (a == "--gamemode") {
      args.gamemode = true;
    } else if (a == "--status-fd") {
      auto v = next();
      if (!v) return std::nullopt;
      args.status_fd = std::atoi(v->c_str());
    } else if (a == "--pre") {
      auto v = next();
      if (!v) return std::nullopt;
      args.pre = *v;
    } else if (a == "--post") {
      auto v = next();
      if (!v) return std::nullopt;
      args.post = *v;
    } else if (a == "--pre-timeout") {
      auto v = next();
      if (!v) return std::nullopt;
      args.pre_timeout_s = std::atoi(v->c_str());
    } else if (a == "--post-timeout") {
      auto v = next();
      if (!v) return std::nullopt;
      args.post_timeout_s = std::atoi(v->c_str());
    } else if (a == "--") {
      ++i;
      break;
    } else {
      return std::nullopt;
    }
  }
  for (; i < argc; ++i) args.game_argv.emplace_back(argv[i]);
  if (args.game_id.empty() || args.database.empty() || args.game_argv.empty()) return std::nullopt;
  return args;
}

void WriteStatus(int fd, std::string_view line) {
  if (fd < 0) return;
  std::size_t written = 0;
  while (written < line.size()) {
    const ssize_t n = ::write(fd, line.data() + written, line.size() - written);
    if (n <= 0) return;
    written += static_cast<std::size_t>(n);
  }
}

struct ScriptResult {
  int exit_code = -1;
  bool timed_out = false;
  std::string output;
};

// Runs `sh -c script` with a wall-clock timeout, SIGKILLing the group if it
// hangs -- pre/post must never wedge the launch or the game's exit.
// Separate from runner::RunAndWait, which has no timeout.
ScriptResult RunScriptWithTimeout(const std::string& script, int timeout_s) {
  ScriptResult result;
  int pipe_fds[2];
  if (::pipe2(pipe_fds, O_CLOEXEC) != 0) return result;

  const pid_t pid = fork();
  if (pid < 0) {
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    return result;
  }
  if (pid == 0) {
    ::setpgid(0, 0);
    ::dup2(pipe_fds[1], STDOUT_FILENO);
    ::dup2(pipe_fds[1], STDERR_FILENO);
    ::execl("/bin/sh", "sh", "-c", script.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  ::close(pipe_fds[1]);

  // Non-blocking so the read loop below can interleave with the WNOHANG
  // waitpid poll instead of a chatty-but-hung script wedging the read past
  // the timeout.
  const int flags = ::fcntl(pipe_fds[0], F_GETFL, 0);
  ::fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
  int status = 0;
  bool exited = false;
  char buffer[4096];
  while (std::chrono::steady_clock::now() < deadline) {
    ssize_t n;
    while ((n = ::read(pipe_fds[0], buffer, sizeof(buffer))) > 0) result.output.append(buffer, static_cast<std::size_t>(n));
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      exited = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (!exited) {
    result.timed_out = true;
    ::kill(-pid, SIGKILL);
    ::waitpid(pid, &status, 0);
  }
  ssize_t n;
  while ((n = ::read(pipe_fds[0], buffer, sizeof(buffer))) > 0) result.output.append(buffer, static_cast<std::size_t>(n));
  ::close(pipe_fds[0]);

  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

// Rotates at session start, not live: renames the existing log to .1,
// dropping whatever .1 was already there -- or dropping the current log
// outright if it's already over the cap. Caps disk use at ~2x max_mb.
void RotateLog(const std::filesystem::path& log_path, int max_mb) {
  std::error_code ec;
  if (!std::filesystem::exists(log_path, ec)) return;
  const auto size = std::filesystem::file_size(log_path, ec);
  const std::string previous = log_path.string() + ".1";
  std::filesystem::remove(previous, ec);
  if (!ec && size <= static_cast<std::uintmax_t>(max_mb) * 1024 * 1024) {
    std::filesystem::rename(log_path, previous, ec);
  } else {
    std::filesystem::remove(log_path, ec);
  }
}

void WriteLogLine(int fd, std::string_view line) {
  if (fd < 0) return;
  std::size_t written = 0;
  while (written < line.size()) {
    const ssize_t n = ::write(fd, line.data() + written, line.size() - written);
    if (n <= 0) return;
    written += static_cast<std::size_t>(n);
  }
}

// Classic setproctitle trick: overwrites argv's own backing memory (one
// contiguous block on Linux, never written past its original end), so
// `ps`/htop show the game instead of the full flag wall. Doesn't touch comm
// (no prctl(PR_SET_NAME)) -- mira-run stays greppable via `pgrep mira-run`.
void SetProcessTitle(int argc, char** argv, const std::string& title) {
  if (argc <= 0) return;
  char* const start = argv[0];
  char* const last_arg_end = argv[argc - 1] + std::strlen(argv[argc - 1]);
  const auto available = static_cast<std::size_t>(last_arg_end - start);
  if (available == 0) return;

  const std::size_t to_copy = std::min(title.size(), available - 1);
  std::memcpy(start, title.data(), to_copy);
  std::memset(start + to_copy, 0, available - to_copy);
}

}  // namespace

int main(int argc, char** argv) {
  const auto parsed = ParseArgs(argc, argv);
  if (!parsed) {
    std::cerr << "mira-run: usage: mira-run --game-id ID --database FILE --status-fd N "
                 "[--pre CMD] [--post CMD] [--pre-timeout S] [--post-timeout S] -- <argv...>\n";
    return 2;
  }
  const Args& args = *parsed;
  SetProcessTitle(argc, argv, "mira-run: " + args.game_id);
  // Otherwise a pre-script that backgrounds something inherits the pipe and holds mirad waiting for EOF.
  if (args.status_fd >= 0) ::fcntl(args.status_fd, F_SETFD, FD_CLOEXEC);

  // Computed before --pre runs so it can ride along on the "ok" status
  // message: with the game id, it keys this launch's session record.
  const auto monotonic_start = std::chrono::steady_clock::now();
  const std::int64_t started_at = model::NowSeconds();

  // Opened before --pre runs so its output lands in the same file. A log
  // that can't be opened just means no logging this session, not a failed
  // launch.
  int log_fd = -1;
  if (!args.log_file.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(args.log_file.parent_path(), ec);
    RotateLog(args.log_file, args.log_max_mb);
    log_fd = ::open(args.log_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (log_fd < 0) {
      std::cerr << "mira-run: could not open log file " << args.log_file << ": " << std::strerror(errno) << "\n";
    } else {
      WriteLogLine(log_fd, std::format("[mira-run] session start, game_id={}\n", args.game_id));
    }
  }

  if (!args.pre.empty()) {
    const ScriptResult pre = RunScriptWithTimeout(args.pre, args.pre_timeout_s);
    if (log_fd >= 0) {
      WriteLogLine(log_fd, std::format("[mira-run] pre_script (exit {}):\n{}\n", pre.exit_code, pre.output));
    }
    if (pre.timed_out) {
      WriteStatus(args.status_fd, "pre_timeout\n");
      return 1;
    }
    if (pre.exit_code != 0) {
      WriteStatus(args.status_fd, std::format("pre_failed\n{}", pre.output));
      return 1;
    }
  }
  WriteStatus(args.status_fd, std::format("ok\n{}\n", started_at));
  if (args.status_fd >= 0) ::close(args.status_fd);

  proc::SessionRecord record;
  record.game_id = args.game_id;
  record.wrapper_pid = ::getpid();
  record.started_at = started_at;

  if (log_fd >= 0) {
    std::string argv_line = "[mira-run] launching:";
    for (const std::string& a : args.game_argv) argv_line += " " + a;
    argv_line += "\n";
    WriteLogLine(log_fd, argv_line);
  }

  std::vector<char*> game_argv;
  game_argv.reserve(args.game_argv.size() + 1);
  for (const std::string& a : args.game_argv) game_argv.push_back(const_cast<char*>(a.c_str()));
  game_argv.push_back(nullptr);

  const pid_t game_pid = fork();
  if (game_pid < 0) {
    // fork() itself failed -- a genuine launch failure, not a bookkeeping one.
    record.finished = true;
    record.launch_error = std::strerror(errno);
    record.exit_code = 127;
    record.ended_at = model::NowSeconds();
    [[maybe_unused]] auto _ = proc::WriteSessionRecord(args.database, record);
    std::cerr << "mira-run: fork failed: " << std::strerror(errno) << "\n";
    return 1;
  }
  if (game_pid == 0) {
    // No setpgid(0, 0) here: the game inherits mira-run's own process group
    // (set by mirad's spawn -- see runner::SpawnDetachedWithStatus), so
    // Stop()'s kill(-pid) reaches the whole tree, mira-run included.
    if (log_fd >= 0) {
      ::dup2(log_fd, STDOUT_FILENO);
      ::dup2(log_fd, STDERR_FILENO);
    }
    ::execvp(game_argv[0], game_argv.data());
    // Only reached if exec itself failed; the log is the one place that can say why.
    WriteLogLine(log_fd, std::format("[mira-run] could not start \"{}\": {}\n", game_argv[0], std::strerror(errno)));
    _exit(127);
  }

  record.game_pid = game_pid;
  // Best-effort: the game is already running regardless of whether this
  // write succeeds.
  [[maybe_unused]] auto write_start = proc::WriteSessionRecord(args.database, record);

  if (args.gamemode) gamemode::RegisterGame(game_pid);

  // mira-run must survive mirad's group-wide kill(-pid) to still run --post
  // and write the final record -- the game gets the same signal directly
  // (same shared group), so no forwarding needed here, just surviving it.
  struct sigaction sa {};
  sa.sa_handler = SIG_IGN;
  ::sigemptyset(&sa.sa_mask);
  ::sigaction(SIGTERM, &sa, nullptr);
  ::sigaction(SIGINT, &sa, nullptr);

  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(game_pid, &status, 0);
  } while (waited < 0 && errno == EINTR);

  if (args.gamemode) gamemode::UnregisterGame(game_pid);

  record.finished = true;
  record.ended_at = model::NowSeconds();
  // Monotonic, not wall-clock: immune to a clock jump mid-session.
  record.duration_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - monotonic_start).count();
  if (WIFEXITED(status)) {
    record.exit_code = WEXITSTATUS(status);
    if (record.exit_code == 127) {
      record.launch_error = std::format("could not start \"{}\"", args.game_argv.front());
    }
  } else if (WIFSIGNALED(status)) {
    record.signal = WTERMSIG(status);
  }

  if (log_fd >= 0) {
    WriteLogLine(log_fd, std::format("[mira-run] game exited: exit_code={} signal={} duration={}s\n",
                                     record.exit_code, record.signal, record.duration_seconds));
  }

  if (!args.post.empty()) {
    const ScriptResult post = RunScriptWithTimeout(args.post, args.post_timeout_s);
    record.post_exit_code = post.exit_code;
    record.post_timed_out = post.timed_out;
    if (log_fd >= 0) {
      WriteLogLine(log_fd, std::format("[mira-run] post_script (exit {}):\n{}\n", post.exit_code, post.output));
    }
  }

  [[maybe_unused]] auto write_end = proc::WriteSessionRecord(args.database, record);
  if (log_fd >= 0) ::close(log_fd);
  return 0;
}
