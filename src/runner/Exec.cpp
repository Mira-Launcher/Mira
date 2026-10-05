#include "runner/Exec.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <mutex>
#include <vector>

#include "core/Lane.h"
#include "core/Strings.h"

extern char** environ;

namespace mira::runner {

std::optional<std::string> FindOnPath(std::string_view name) {
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) return std::nullopt;

  for (const std::string& dir : strings::Split(path_env, ':')) {
    if (dir.empty()) continue;
    const std::filesystem::path candidate = std::filesystem::path(dir) / name;
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec) && access(candidate.c_str(), X_OK) == 0) {
      return candidate.string();
    }
  }
  return std::nullopt;
}

std::optional<std::string> ResolveSiblingBinary(const std::filesystem::path& own_binary_dir, std::string_view name) {
  if (!own_binary_dir.empty()) {
    const std::filesystem::path candidate = own_binary_dir / name;
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec)) return candidate.string();
  }
  return FindOnPath(name);
}

namespace {

// mirad blocks SIGINT/SIGTERM on every thread (see mirad_main.cpp), and a
// child inherits the forking thread's mask across exec. Unblocked in the
// child so a game can be stopped with SIGTERM. Async-signal-safe.
void UnblockSignals() {
  sigset_t none;
  sigemptyset(&none);
  sigprocmask(SIG_SETMASK, &none, nullptr);
}

// Command.env is an overlay on the daemon's own environment, not a
// replacement, so merge them for the child process.
std::vector<std::string> MergedEnv(const Command& command) {
  std::vector<std::string> merged;
  for (char** e = environ; *e != nullptr; ++e) merged.emplace_back(*e);
  for (const auto& [key, value] : command.env) merged.push_back(key + "=" + value);
  return merged;
}

// argv/envp built before fork() and only indexed into afterwards; see the
// deadlock note in RunAndWait.
struct PreparedCommand {
  std::vector<std::string> env_strings;
  std::vector<char*> argv;
  std::vector<char*> envp;
  std::string cwd;
};

PreparedCommand Prepare(const Command& command) {
  PreparedCommand prepared;
  prepared.env_strings = MergedEnv(command);
  prepared.argv.reserve(command.argv.size() + 1);
  for (const std::string& arg : command.argv) {
    prepared.argv.push_back(const_cast<char*>(arg.c_str()));
  }
  prepared.argv.push_back(nullptr);
  prepared.envp.reserve(prepared.env_strings.size() + 1);
  for (const std::string& entry : prepared.env_strings) {
    prepared.envp.push_back(const_cast<char*>(entry.c_str()));
  }
  prepared.envp.push_back(nullptr);
  prepared.cwd = command.cwd.string();
  return prepared;
}

}  // namespace

Result<pid_t> SpawnDetached(const Command& command) {
  if (command.argv.empty()) return Err("exec_empty_argv", "no command to run");
  PreparedCommand prepared = Prepare(command);

  const pid_t pid = fork();
  if (pid < 0) return Err("exec_fork_failed", std::strerror(errno));
  if (pid == 0) {
    // Child: async-signal-safe calls only. stdout/stderr are inherited.
    // Its own process group, so stopping the game can signal the whole tree:
    // a real launch is umu -> proton -> wine -> game.exe, and signalling just
    // the direct child leaves the actual game running.
    setpgid(0, 0);
    UnblockSignals();
    if (!prepared.cwd.empty() && chdir(prepared.cwd.c_str()) != 0) _exit(127);
    execvpe(prepared.argv[0], prepared.argv.data(), prepared.envp.data());
    _exit(127);
  }
  return pid;
}

Result<pid_t> SpawnDetachedWithStatus(const Command& command, int& status_read_fd) {
  if (command.argv.empty()) return Err("exec_empty_argv", "no command to run");
  PreparedCommand prepared = Prepare(command);

  int pipe_fds[2];
  if (pipe2(pipe_fds, O_CLOEXEC) != 0) return Err("exec_pipe_failed", std::strerror(errno));

  const pid_t pid = fork();
  if (pid < 0) {
    const std::string message = std::strerror(errno);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return Err("exec_fork_failed", message);
  }
  if (pid == 0) {
    // Child: async-signal-safe calls only. The write end is moved onto a
    // fixed, known fd (3) so the spawned process can be told about it as a
    // plain CLI flag (--status-fd 3) rather than needing to inherit an
    // unpredictable fd number.
    setpgid(0, 0);
    UnblockSignals();
    dup2(pipe_fds[1], 3);
    close(pipe_fds[0]);
    if (pipe_fds[1] != 3) close(pipe_fds[1]);
    if (!prepared.cwd.empty() && chdir(prepared.cwd.c_str()) != 0) _exit(127);
    execvpe(prepared.argv[0], prepared.argv.data(), prepared.envp.data());
    _exit(127);
  }
  close(pipe_fds[1]);
  status_read_fd = pipe_fds[0];
  return pid;
}

Result<ExecResult> RunAndWait(const Command& command, const OutputFn& on_output) {
  if (command.argv.empty()) return Err("exec_empty_argv", "no command to run");

  // Everything the child needs is built *before* fork(). mirad forks from
  // threads (the scan thread, httplib workers) while others run, and only
  // async-signal-safe calls are legal in the child: a malloc there deadlocks
  // forever if another thread happened to hold the arena lock at fork time,
  // and the parent then blocks forever too, since its read() waits for an
  // EOF the wedged child will never deliver.
  const std::vector<std::string> env_strings = MergedEnv(command);
  std::vector<char*> argv;
  argv.reserve(command.argv.size() + 1);
  for (const std::string& arg : command.argv) argv.push_back(const_cast<char*>(arg.c_str()));
  argv.push_back(nullptr);
  std::vector<char*> envp;
  envp.reserve(env_strings.size() + 1);
  for (const std::string& entry : env_strings) envp.push_back(const_cast<char*>(entry.c_str()));
  envp.push_back(nullptr);
  const std::string cwd = command.cwd.string();

  // O_CLOEXEC: two RunAndWait calls can overlap (a scan thread provisioning
  // while an HTTP request probes `wine --version`). Without it, one child
  // inherits the other's write end and holds the reader open until it exits.
  int pipe_fds[2];
  if (pipe2(pipe_fds, O_CLOEXEC) != 0) return Err("exec_pipe_failed", std::strerror(errno));

  const pid_t pid = fork();
  if (pid < 0) {
    const std::string message = std::strerror(errno);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return Err("exec_fork_failed", message);
  }

  if (pid == 0) {
    // Child: async-signal-safe calls only from here down. dup2 clears
    // O_CLOEXEC on the copies, so stdout/stderr survive exec while the
    // originals close themselves.
    UnblockSignals();
    // Its own group, so a timeout or shutdown also kills whatever it spawned.
    setpgid(0, 0);
    dup2(pipe_fds[1], STDOUT_FILENO);
    dup2(pipe_fds[1], STDERR_FILENO);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
    execvpe(argv[0], argv.data(), envp.data());
    _exit(127);  // only reached if exec failed
  }

  // Parent.
  close(pipe_fds[1]);
  setpgid(pid, pid);
  const std::stop_token stop = ThisTaskStop();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(command.timeout_s);
  bool timed_out = false;
  bool stopped = false;
  ExecResult result;
  char buffer[4096];
  for (;;) {
    if (command.timeout_s > 0 || stop.stop_possible()) {
      if (stop.stop_requested()) {
        stopped = true;
        break;
      }
      long long wait_ms = 250;
      if (command.timeout_s > 0) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
          timed_out = true;
          break;
        }
        if (!stop.stop_possible() || left < wait_ms) wait_ms = left;
      }
      pollfd ready{.fd = pipe_fds[0], .events = POLLIN, .revents = 0};
      const int polled = poll(&ready, 1, static_cast<int>(wait_ms));
      if (polled < 0 && errno != EINTR) break;
      if (polled <= 0) continue;
    }
    const ssize_t n = read(pipe_fds[0], buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    result.output.append(buffer, static_cast<size_t>(n));
    if (on_output) on_output(std::string_view(buffer, static_cast<size_t>(n)));
  }
  close(pipe_fds[0]);
  if (timed_out || stopped) {
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
  }

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) return Err("exec_wait_failed", std::strerror(errno));
  if (stopped && ThisTaskCancelled()) return Err("cancelled", std::format("{} was cancelled", command.argv[0]));
  if (stopped) return Err("shutting_down", std::format("{} was stopped because mirad is shutting down", command.argv[0]));
  if (timed_out) {
    return Err("exec_timeout", std::format("{} gave no result within {}s", command.argv[0], command.timeout_s));
  }
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

std::string ToolVersion(const std::string& path) {
  static std::mutex mutex;
  static std::map<std::filesystem::path, std::pair<std::filesystem::file_time_type, std::string>> cache;
  std::error_code ec;
  const std::filesystem::path resolved = std::filesystem::canonical(path, ec);
  const std::filesystem::file_time_type mtime =
      ec ? std::filesystem::file_time_type() : std::filesystem::last_write_time(resolved, ec);
  if (!ec) {
    const std::lock_guard lock(mutex);
    if (const auto found = cache.find(resolved); found != cache.end() && found->second.first == mtime) {
      return found->second.second;
    }
  }
  Command command;
  command.argv = {path, "--version"};
  command.timeout_s = 15;  // a hung tool must not block its status
  const auto result = RunAndWait(command);
  std::string version = result && result->exit_code == 0 ? strings::Trim(result->output) : std::string();
  if (!ec && !version.empty()) {
    const std::lock_guard lock(mutex);
    cache[resolved] = {mtime, version};
  }
  return version;
}

std::string CurlConfigLine(std::string_view name, std::string_view value) {
  std::string line = std::string(name) + " = \"";
  for (const char c : value) {
    if (c == '\\' || c == '"') line += '\\';
    line += c;
  }
  return line + "\"\n";
}

Result<ExecResult> RunCurlWithSecrets(const std::vector<std::string>& args, std::string_view secret_config) {
  std::string path = (std::filesystem::temp_directory_path() / "mira-curl-XXXXXX").string();
  const int fd = mkostemp(path.data(), O_CLOEXEC);  // created 0600
  if (fd < 0) return Err("exec_temp_failed", std::strerror(errno));
  const bool written = write(fd, secret_config.data(), secret_config.size()) ==
                       static_cast<ssize_t>(secret_config.size());
  close(fd);
  Result<ExecResult> result = Err("exec_temp_failed", "couldn't write curl's config");
  if (written) {
    Command command;
    command.argv = {"curl", "-K", path};
    command.argv.insert(command.argv.end(), args.begin(), args.end());
    result = RunAndWait(command);
  }
  unlink(path.c_str());
  return result;
}

Result<void> Extract(const std::filesystem::path& archive, const std::filesystem::path& out_dir) {
  std::string name = archive.filename().string();
  std::ranges::transform(name, name.begin(), [](unsigned char c) { return std::tolower(c); });
  const bool tarball = name.ends_with(".tar") || name.ends_with(".tgz") || name.find(".tar.") != std::string::npos;

  Command command;
  if (tarball) {
    command.argv = {"tar", "-xf", archive.string(), "-C", out_dir.string()};
  } else {
    std::optional<std::string> seven_zip = FindOnPath("7z");
    if (!seven_zip) seven_zip = FindOnPath("7zz");
    if (!seven_zip) return Err("extractor_missing", "install 7zip to extract archives");
    command.argv = {*seven_zip, "x", "-y", "-bso0", "-bsp0", archive.string(), "-o" + out_dir.string()};
  }
  const Result<ExecResult> result = RunAndWait(command);
  if (!result) return std::unexpected(result.error());
  if (result->exit_code != 0) {
    return Err("extract_failed", std::format("{} exited {}: {}", command.argv[0], result->exit_code, result->output));
  }
  return {};
}

}  // namespace mira::runner
