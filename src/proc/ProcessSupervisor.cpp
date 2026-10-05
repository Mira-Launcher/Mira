#include "proc/ProcessSupervisor.h"

#include <dirent.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <set>

#include "core/Log.h"
#include "proc/ProcessIndex.h"
#include "proc/Session.h"
#include "runner/Exec.h"

namespace mira::proc {
namespace {

using nlohmann::json;

constexpr auto kPollInterval = std::chrono::milliseconds(1000);

// Playtime is written back as it accrues, not only at exit: if mirad is
// killed or crashes while a game is running, its watcher dies with it, and
// anything not yet checkpointed is lost. This bounds that loss to a minute
// instead of the whole session.
constexpr std::int64_t kCheckpointSeconds = 60;

// A crashed game.state's hint and fix: its log, and for a Windows game another runner.
void AddCrashHelp(json& event, const std::string& game_id, bool windows) {
  event["hint"] = windows ? "The game's log usually says why. Another runner in the game's settings often helps."
                          : "The game's log usually says why.";
  event["fix"] = {{"kind", "game"}, {"target", game_id}, {"step", "log"}};
}

void RunScript(const std::string& script, const std::string& game_id, const char* which) {
  if (script.empty()) return;
  Command command;
  command.argv = {"sh", "-c", script};
  if (auto result = runner::RunAndWait(command); !result || result->exit_code != 0) {
    log::Warn("launch.{}_script for {} failed: {}", which, game_id,
             !result ? result.error().message : std::format("exited {}", result->exit_code));
  }
}

// Every pid under this UID whose /proc/<pid>/environ carries
// SteamAppId=<appid> or SteamGameId=<appid> -- the whole subtree Steam's
// launch produces (reaper, pressure-vessel, proton, the game itself), not
// one specific process, since which of those is "the" game process varies
// by title and none of them is a child of this daemon either way.
std::set<pid_t> FindSteamProcesses(const std::string& appid) {
  std::set<pid_t> found;
  const std::string marker_app = "SteamAppId=" + appid;
  const std::string marker_game = "SteamGameId=" + appid;

  DIR* proc_dir = ::opendir("/proc");
  if (!proc_dir) return found;
  while (const dirent* entry = ::readdir(proc_dir)) {
    const std::string name = entry->d_name;
    if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0]))) continue;
    const pid_t pid = std::atoi(name.c_str());

    std::ifstream environ_file("/proc/" + name + "/environ", std::ios::binary);
    if (!environ_file) continue;  // gone, or not our own process (permission denied)
    const std::string environ((std::istreambuf_iterator<char>(environ_file)),
                              std::istreambuf_iterator<char>());
    // environ is NUL-separated, not newline-separated -- a plain substring
    // search still works since neither marker can span a NUL boundary by
    // construction (both are single "KEY=VALUE" entries).
    if (environ.find(marker_app) != std::string::npos || environ.find(marker_game) != std::string::npos) {
      found.insert(pid);
    }
  }
  ::closedir(proc_dir);
  return found;
}

// Signals a game's process group plus every process sharing its prefix.
void SignalGame(pid_t pid, const std::string& data_dir, int signal_number) {
  if (pid > 0) {
    ::kill(-pid, signal_number);
    ::kill(pid, signal_number);
  }
  for (pid_t found : FindPrefixProcesses(data_dir)) ::kill(found, signal_number);
}

bool AnyAlive(const std::set<pid_t>& pids) {
  for (pid_t pid : pids) {
    if (::kill(pid, 0) == 0) return true;
  }
  return false;
}

// Calls fn(pid) for every process whose WINEPREFIX/STEAM_COMPAT_DATA_PATH is
// data_dir or under it. Entry-by-entry, not substring: "…/prefix/animal"
// must not match "…/prefix/animal-well". umu rewrites WINEPREFIX to
// "<data_dir>/pfx/".
template <typename Fn>
void ForEachPrefixProcess(const std::string& data_dir, Fn&& fn) {
  const auto matches = [&data_dir](std::string_view value) {
    if (value == data_dir) return true;
    return value.size() > data_dir.size() && value.starts_with(data_dir) && value[data_dir.size()] == '/';
  };

  DIR* proc_dir = ::opendir("/proc");
  if (!proc_dir) return;
  while (const dirent* entry = ::readdir(proc_dir)) {
    const std::string name = entry->d_name;
    if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0]))) continue;

    std::ifstream environ_file("/proc/" + name + "/environ", std::ios::binary);
    if (!environ_file) continue;  // gone, or not our own process
    const std::string environ((std::istreambuf_iterator<char>(environ_file)),
                              std::istreambuf_iterator<char>());

    for (std::size_t start = 0; start < environ.size();) {
      const std::size_t end = environ.find('\0', start);
      const std::string_view item(environ.data() + start,
                                  (end == std::string::npos ? environ.size() : end) - start);
      start = (end == std::string::npos) ? environ.size() : end + 1;

      const std::size_t equals = item.find('=');
      if (equals == std::string_view::npos) continue;
      const std::string_view key = item.substr(0, equals);
      if (key != "WINEPREFIX" && key != "STEAM_COMPAT_DATA_PATH") continue;
      if (matches(item.substr(equals + 1))) {
        fn(name);
        break;
      }
    }
  }
  ::closedir(proc_dir);
}

// Every process of the game, for signalling it.
std::set<pid_t> FindExternal(const ExternalMatch& match) {
  return match.appid.empty() ? FindDirProcesses(match.data_dir, match.win_dir) : FindSteamProcesses(match.appid);
}

// Whether the game is running, from a cached index: a Steam game by the
// reaper Steam wraps it in, a launcher game by its folder in the prefix.
std::set<pid_t> MatchExternal(const ProcessIndex& index, const ExternalMatch& match) {
  std::set<pid_t> found;
  for (const auto& [pid, info] : index.Processes()) {
    const bool hit = match.appid.empty()
                         ? InPrefix(info.prefix, match.data_dir) && info.argv0.starts_with(match.win_dir + "/")
                         : info.steam_launch == match.appid;
    if (hit) found.insert(pid);
  }
  return found;
}

}  // namespace

// Every pid whose WINEPREFIX/STEAM_COMPAT_DATA_PATH points at data_dir.
//
// The process group alone isn't enough, on Proton/Wine, setsid()/setpgid()
// during startup leaves the group with just umu-run by the time a game is on screen

std::set<pid_t> FindPrefixProcesses(const std::string& data_dir) {
  std::set<pid_t> found;
  if (data_dir.empty()) return found;
  ForEachPrefixProcess(data_dir, [&](const std::string& pid) { found.insert(std::atoi(pid.c_str())); });
  return found;
}

std::set<pid_t> FindDirProcesses(const std::string& data_dir, const std::string& win_dir) {
  std::set<pid_t> found;
  if (data_dir.empty() || win_dir.empty()) return found;
  ForEachPrefixProcess(data_dir, [&](const std::string& pid) {
    std::ifstream cmdline_file("/proc/" + pid + "/cmdline", std::ios::binary);
    std::string argv0;
    std::getline(cmdline_file, argv0, '\0');
    std::ranges::replace(argv0, '\\', '/');
    for (char& ch : argv0) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (argv0.starts_with(win_dir + "/")) found.insert(std::atoi(pid.c_str()));
  });
  return found;
}

ProcessSupervisor::ProcessSupervisor(store::GameStore& games, api::EventBus& events,
                                     std::int64_t stop_timeout_s)
    : games_(games), events_(events), stop_timeout_s_(stop_timeout_s) {}

ProcessSupervisor::~ProcessSupervisor() {
  {
    const std::lock_guard lock(stop_mutex_);
    stopping_.store(true, std::memory_order_relaxed);
  }
  stop_wake_.notify_all();
  // Games are deliberately left running: quitting the daemon shouldn't kill
  // what the player is playing. The watchers just stop watching.
  std::map<std::string, std::thread> watchers;
  std::vector<std::thread> retired;
  {
    std::lock_guard lock(mutex_);
    watchers.swap(watchers_);
    retired.swap(retired_);
  }
  for (auto& [id, thread] : watchers) {
    if (thread.joinable()) thread.join();
  }
  for (std::thread& thread : retired) {
    if (thread.joinable()) thread.join();
  }
}

bool ProcessSupervisor::PollWaitStopping() {
  std::unique_lock lock(stop_mutex_);
  const auto stopped = [this] { return stopping_.load(std::memory_order_relaxed); };
  return stop_wake_.wait_for(lock, kPollInterval, stopped);
}

// A record still unfinished once its mira-run is gone (killed with the game) has only its start; close it out with the elapsed time.
static void CloseOutUnfinished(SessionRecord& record) {
  if (record.finished) return;
  const std::int64_t now = model::NowSeconds();
  record.finished = true;
  record.ended_at = now;
  record.duration_seconds = std::max<std::int64_t>(0, now - record.started_at);
}

void ProcessSupervisor::AdoptWatcher(const std::string& game_id, std::thread watcher) {
  // The previous watcher for this id has erased itself from running_, but may
  // still be recording its exit, so it is kept to be joined, not detached.
  if (auto stale = watchers_.find(game_id); stale != watchers_.end()) {
    if (stale->second.joinable()) retired_.push_back(std::move(stale->second));
    watchers_.erase(stale);
  }
  watchers_[game_id] = std::move(watcher);
}

Result<void> ProcessSupervisor::Launch(const model::Game& game, const Command& command,
                                       std::string post_script) {
  {
    std::lock_guard lock(mutex_);
    if (running_.contains(game.id)) {
      return Err("already_running", std::format("\"{}\" is already running", game.id));
    }
  }

  auto pid = runner::SpawnDetached(command);
  if (!pid) return std::unexpected(pid.error());

  const std::int64_t started_at = model::NowSeconds();
  {
    std::lock_guard lock(mutex_);
    running_[game.id] = *pid;
    prefixes_[game.id] = game.data_dir;  // for Stop(), see FindPrefixProcesses
    AdoptWatcher(game.id,
                 std::thread(&ProcessSupervisor::Watch, this, game.id, *pid, started_at, std::move(post_script)));
  }

  // Written now, not at exit: this is "when you last started playing", and
  // recording it immediately means it survives the daemon dying mid-session.
  auto stamped = games_.Update(game.id, [&](model::Game& stored) {
    stored.last_played_at = started_at;
  });
  if (!stamped) {
    log::Error("failed to record launch time for {}: {}", game.id, stamped.error().message);
  }

  log::Info("launched {} (pid {})", game.id, *pid);
  // Full record (see the exit events below for why) so a listener never
  // has to relist just to pick up last_played_at.
  json event = stamped ? model::ToJson(*stamped) : json{{"id", game.id}};
  event["state"] = "running";
  event["pid"] = *pid;
  events_.Publish("game.state", std::move(event));
  return {};
}

Result<void> ProcessSupervisor::LaunchWrapped(const model::Game& game, pid_t wrapper_pid,
                                              std::filesystem::path session_path) {
  {
    std::lock_guard lock(mutex_);
    if (running_.contains(game.id)) {
      return Err("already_running", std::format("\"{}\" is already running", game.id));
    }
    // wrapper_pid, not the game's own pid: it's the process group leader,
    // so Stop()'s kill(-pid) reaches mira-run and the game together.
    running_[game.id] = wrapper_pid;
    prefixes_[game.id] = game.data_dir;
    AdoptWatcher(game.id,
                 std::thread(&ProcessSupervisor::WatchWrapped, this, game.id, wrapper_pid, std::move(session_path)));
  }

  const std::int64_t started_at = model::NowSeconds();
  auto stamped = games_.Update(game.id, [&](model::Game& stored) { stored.last_played_at = started_at; });
  if (!stamped) {
    log::Error("failed to record launch time for {}: {}", game.id, stamped.error().message);
  }

  log::Info("launched {} (wrapper pid {})", game.id, wrapper_pid);
  json event = stamped ? model::ToJson(*stamped) : json{{"id", game.id}};
  event["state"] = "running";
  event["pid"] = wrapper_pid;
  events_.Publish("game.state", std::move(event));
  return {};
}

Result<void> ProcessSupervisor::Stop(const std::string& game_id) {
  pid_t pid = 0;
  std::string data_dir;
  std::optional<ExternalMatch> match;
  {
    std::lock_guard lock(mutex_);
    const auto it = running_.find(game_id);
    if (it == running_.end()) {
      return Err("not_running", std::format("\"{}\" is not running", game_id));
    }
    pid = it->second;
    if (const auto prefix = prefixes_.find(game_id); prefix != prefixes_.end()) {
      data_dir = prefix->second;
    }
    if (const auto external = external_.find(game_id); external != external_.end()) {
      match = external->second;
    }
  }
  // 0 is TrackSteamLaunch's "reserved, not confirmed yet" sentinel: kill(0,
  // ...)/kill(-0, ...) both mean "signal every process in the caller's own
  // group" per POSIX, which would hit mirad itself. A real pid is always > 0.
  if (pid <= 0) {
    return Err("not_yet_confirmed",
              std::format("\"{}\" was launched but its process isn't confirmed yet. Try again shortly",
                          game_id));
  }
  // Group (see runner::SpawnDetached's setpgid note) plus the prefix, plus
  // (for a Steam-launched game) every pid FindSteamProcesses finds under its
  // appid: that tree is Steam's own (reaper/pressure-vessel/proton/the game),
  // not a child of mirad and not one shared process group, so the group
  // signal above only ever reaches whichever single pid WatchExternal recorded.
  const std::set<pid_t> in_prefix = FindPrefixProcesses(data_dir);
  const std::set<pid_t> in_steam_tree = match ? FindExternal(*match) : std::set<pid_t>();
  bool signalled = ::kill(-pid, SIGTERM) == 0 || ::kill(pid, SIGTERM) == 0;
  for (pid_t found : in_prefix) { ::kill(found, SIGTERM); signalled = true; }
  for (pid_t found : in_steam_tree) { ::kill(found, SIGTERM); signalled = true; }
  if (!signalled) {
    return Err("stop_failed", std::format("could not signal pid {}", pid));
  }
  {
    std::lock_guard lock(mutex_);
    // Skipped if the game exited meanwhile: its watcher already cleaned up, and these would outlive it.
    if (running_.contains(game_id)) {
      kill_deadlines_[game_id] = model::NowSeconds() + stop_timeout_s_;
      stop_requested_.insert(game_id);
    }
  }
  return {};
}

std::optional<ProcessSupervisor::Reservation> ProcessSupervisor::Reserve(const std::string& game_id) {
  {
    std::lock_guard lock(mutex_);
    if (running_.contains(game_id) || !reserved_.insert(game_id).second) return std::nullopt;
  }
  return Reservation(*this, game_id);
}

void ProcessSupervisor::Release(const std::string& game_id) {
  std::lock_guard lock(mutex_);
  reserved_.erase(game_id);
}

bool ProcessSupervisor::IsRunning(const std::string& game_id) const {
  std::lock_guard lock(mutex_);
  return running_.contains(game_id);
}

void ProcessSupervisor::Watch(std::string game_id, pid_t pid, std::int64_t started_at,
                              std::string post_script) {
  int status = 0;
  std::int64_t credited = 0;  // seconds already written to the store
  while (!stopping_.load(std::memory_order_relaxed)) {
    const pid_t result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid) break;                    // exited
    if (result < 0) { status = -1; break; }      // vanished; treat as exited

    // A game that ignores SIGTERM gets SIGKILL once its deadline passes.
    {
      std::lock_guard lock(mutex_);
      const auto deadline = kill_deadlines_.find(game_id);
      if (deadline != kill_deadlines_.end() && model::NowSeconds() >= deadline->second) {
        log::Warn("{} ignored SIGTERM; sending SIGKILL", game_id);
        const auto prefix = prefixes_.find(game_id);
        SignalGame(pid, prefix == prefixes_.end() ? std::string() : prefix->second, SIGKILL);
        kill_deadlines_.erase(deadline);
      }
    }

    const std::int64_t elapsed = model::NowSeconds() - started_at;
    if (elapsed - credited >= kCheckpointSeconds) {
      const std::int64_t delta = elapsed - credited;
      auto checkpoint = games_.Update(game_id, [&](model::Game& game) {
        game.play_seconds += delta;
      });
      if (checkpoint) credited = elapsed;
    }
    if (PollWaitStopping()) break;
  }
  if (stopping_.load(std::memory_order_relaxed)) return;  // daemon going away

  const std::int64_t ended_at = model::NowSeconds();
  const std::int64_t played = ended_at > started_at ? ended_at - started_at : 0;

  // A crash is a different outcome from a clean exit and has to be reported
  // as one: killed by a signal, or exited non-zero. Playtime is recorded
  // either way, since the session still happened.
  const bool signalled = WIFSIGNALED(status);
  const int signal_number = signalled ? WTERMSIG(status) : 0;
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  // SIGTERM/SIGINT is how Stop() asks a game to quit, so that's a stop, not
  // a crash.
  bool requested_stop;
  {
    std::lock_guard lock(mutex_);
    requested_stop = stop_requested_.contains(game_id);
  }
  const bool crashed = !requested_stop && ((signalled && signal_number != SIGTERM) ||
                                           (!signalled && exit_code != 0));

  std::string error;
  if (crashed) {
    error = signalled ? std::format("Crashed on signal {} ({}) after {}s", signal_number,
                                    ::strsignal(signal_number), played)
                      : std::format("Exited with code {} after {}s", exit_code, played);
  }

  {
    std::lock_guard lock(mutex_);
    running_.erase(game_id);
    prefixes_.erase(game_id);
    kill_deadlines_.erase(game_id);
    stop_requested_.erase(game_id);
  }

  auto updated = games_.Update(game_id, [&](model::Game& game) {
    game.play_seconds += played - credited;  // the rest was checkpointed already
    // Surfaced by the frontend as "last run didn't go well"; cleared on a
    // clean run so a one-off crash doesn't stick around forever.
    game.last_error = error;
  });
  if (!updated) {
    log::Error("failed to record playtime for {}: {}", game_id, updated.error().message);
  }

  if (crashed) {
    log::Warn("{} {}", game_id, error);
  } else {
    log::Info("{} exited cleanly after {}s", game_id, played);
  }
  // The full updated record rides along on top of the session-only fields
  // below (exit_code, signal, played_seconds are this session's, not the
  // row's running totals) so a listener can patch its one row directly;
  // this used to carry only id/state, forcing a full GET /v1/games relist
  // just to pick up the new play_seconds/last_played_at/last_error.
  json event = updated ? model::ToJson(*updated) : json{{"id", game_id}};
  event["state"] = crashed ? "crashed" : "exited";
  event["exit_code"] = exit_code;
  event["signal"] = signal_number;
  event["played_seconds"] = played;
  event["error"] = error;
  if (crashed) AddCrashHelp(event, game_id, updated && updated->platform == model::Platform::Windows);
  events_.Publish("game.state", std::move(event));
  if (exit_hook_) exit_hook_(game_id);

  RunScript(post_script, game_id, "post");
}

void ProcessSupervisor::WatchWrapped(std::string game_id, pid_t wrapper_pid,
                                     std::filesystem::path session_path) {
  // WNOHANG, not a blocking wait: no playtime checkpointing needed (the
  // session record already survives mirad dying), but Stop()'s SIGKILL
  // escalation via kill_deadlines_ still needs servicing, which a blocking
  // waitpid() would never notice. On shutdown this returns at once: mira-run
  // outlives mirad and its session record is picked up by Reconcile() on the
  // next start.
  int status = 0;
  while (!stopping_.load(std::memory_order_relaxed)) {
    const pid_t waited = ::waitpid(wrapper_pid, &status, WNOHANG);
    if (waited == wrapper_pid) break;
    if (waited < 0 && errno != EINTR) break;  // wrapper_pid vanished; treat as exited

    {
      std::lock_guard lock(mutex_);
      const auto deadline = kill_deadlines_.find(game_id);
      if (deadline != kill_deadlines_.end() && model::NowSeconds() >= deadline->second) {
        log::Warn("{} ignored SIGTERM; sending SIGKILL", game_id);
        const auto prefix = prefixes_.find(game_id);
        SignalGame(wrapper_pid, prefix == prefixes_.end() ? std::string() : prefix->second, SIGKILL);
        kill_deadlines_.erase(deadline);
      }
    }
    if (PollWaitStopping()) break;
  }
  if (stopping_.load(std::memory_order_relaxed)) return;  // daemon going away

  auto record = ReadSessionRecord(session_path);
  {
    std::lock_guard lock(mutex_);
    running_.erase(game_id);
    prefixes_.erase(game_id);
    kill_deadlines_.erase(game_id);
    stop_requested_.erase(game_id);
  }
  if (!record) {
    // mira-run vanished without writing a record (killed before it could
    // fork, or before its first write) -- writes are temp-file-then-rename,
    // so a reader never sees a partial file.
    log::Warn("mira-run for {} exited with no session record ({})", game_id, record.error().message);
    return;
  }
  CloseOutUnfinished(*record);
  FinalizeWrappedSession(game_id, *record, session_path);
}

// Shared by WatchWrapped and Reconcile/WatchReconciledLive: classify the
// record, update the store, publish the event, delete the session file.
void ProcessSupervisor::FinalizeWrappedSession(const std::string& game_id, const proc::SessionRecord& record,
                                               const std::filesystem::path& session_path) {
  // Mirrors Watch()'s classification. A record with no exit info at all
  // (mira-run itself SIGKILLed before finishing) is left un-crashed rather
  // than guessed at; `incomplete` is what actually flags it as suspect.
  const bool crashed = !record.incomplete &&
      ((record.signal != 0 && record.signal != SIGTERM) || (record.signal == 0 && record.exit_code > 0));

  std::string error = record.launch_error;
  if (error.empty() && crashed) {
    error = record.signal != 0
                ? std::format("Crashed on signal {} ({}) after {}s", record.signal, ::strsignal(record.signal),
                              record.duration_seconds)
                : std::format("Exited with code {} after {}s", record.exit_code, record.duration_seconds);
  }
  if (record.incomplete) {
    error = "Mira restarted mid-session; this session's true ending was never observed.";
  }

  auto updated = games_.Update(game_id, [&](model::Game& game) {
    // Counted once even if mirad dies before the session file is removed and finds it again.
    if (record.started_at > game.last_session_at) {
      game.play_seconds += record.duration_seconds;
      game.last_session_at = record.started_at;
    }
    game.last_error = error;
  });
  if (!updated) {
    log::Error("failed to record playtime for {}: {}", game_id, updated.error().message);
  }

  if (crashed) {
    log::Warn("{} {}", game_id, error);
  } else {
    log::Info("{} exited after {}s", game_id, record.duration_seconds);
  }

  json event = updated ? model::ToJson(*updated) : json{{"id", game_id}};
  event["state"] = crashed ? "crashed" : "exited";
  event["exit_code"] = record.exit_code;
  event["signal"] = record.signal;
  event["played_seconds"] = record.duration_seconds;
  event["error"] = error;
  if (crashed) AddCrashHelp(event, game_id, updated && updated->platform == model::Platform::Windows);
  events_.Publish("game.state", std::move(event));
  if (exit_hook_) exit_hook_(game_id);

  // The session file only ever covered the gap until mirad got a chance to see it finished.
  std::error_code ec;
  std::filesystem::remove(session_path, ec);
}

void ProcessSupervisor::WatchReconciledLive(std::string game_id, pid_t wrapper_pid,
                                            std::filesystem::path session_path) {
  // Not this mirad's child, so waitpid() can't work -- poll liveness instead.
  while (!stopping_.load(std::memory_order_relaxed) && ::kill(wrapper_pid, 0) == 0) {
    if (PollWaitStopping()) break;
  }
  if (stopping_.load(std::memory_order_relaxed)) return;  // daemon going away

  auto record = ReadSessionRecord(session_path);
  {
    std::lock_guard lock(mutex_);
    running_.erase(game_id);
    prefixes_.erase(game_id);
    kill_deadlines_.erase(game_id);
    stop_requested_.erase(game_id);
  }
  if (!record) {
    log::Warn("re-adopted mira-run for {} exited with no readable session record ({})", game_id,
             record.error().message);
    return;
  }
  CloseOutUnfinished(*record);
  FinalizeWrappedSession(game_id, *record, session_path);
}

void ProcessSupervisor::Reconcile(const std::filesystem::path& sessions_dir) {
  std::error_code ec;
  if (!std::filesystem::exists(sessions_dir, ec)) return;

  for (const auto& entry :
       std::filesystem::directory_iterator(sessions_dir, std::filesystem::directory_options::skip_permission_denied,
                                           ec)) {
    if (ec) break;
    if (!entry.is_regular_file(ec)) continue;
    const std::filesystem::path path = entry.path();

    auto record = ReadSessionRecord(path);
    if (!record) {
      // A truncated/corrupt leftover from a mirad that died mid-write of
      // its own -- never block startup over it, just drop it and move on.
      log::Warn("skipping unreadable session file {}: {}", path.string(), record.error().message);
      std::error_code rm_ec;
      std::filesystem::remove(path, rm_ec);
      continue;
    }

    if (record->finished) {
      // mira-run had already finished and written the final record, but
      // the mirad that was supposed to notice and archive it died first.
      // Nothing to watch, so just finish the bookkeeping mira-run itself
      // already completed the hard part of.
      FinalizeWrappedSession(record->game_id, *record, path);
      continue;
    }

    const bool wrapper_alive = record->wrapper_pid > 0 && ::kill(record->wrapper_pid, 0) == 0;
    if (wrapper_alive) {
      log::Info("re-adopting live session for {} (mira-run pid {})", record->game_id, record->wrapper_pid);
      std::lock_guard lock(mutex_);
      running_[record->game_id] = record->wrapper_pid;
      AdoptWatcher(record->game_id, std::thread(&ProcessSupervisor::WatchReconciledLive, this, record->game_id,
                                                record->wrapper_pid, path));
    } else {
      log::Warn("session for {} was left behind by a mira-run (pid {}) that's no longer running; closing it out "
               "as incomplete",
               record->game_id, record->wrapper_pid);
      record->incomplete = true;
      CloseOutUnfinished(*record);
      FinalizeWrappedSession(record->game_id, *record, path);
    }
  }
}

Result<void> ProcessSupervisor::TrackSteamLaunch(const model::Game& game, const std::string& appid,
                                                 std::string post_script) {
  // Steam has real startup latency (client wakeup, update checks, Proton's
  // own prefix work) before the game process exists at all.
  ExternalMatch match;
  match.appid = appid;
  match.detect_timeout_s = 60;
  return TrackExternal(game, std::move(match), std::move(post_script));
}

Result<void> ProcessSupervisor::TrackLauncherLaunch(const model::Game& game, const std::string& win_dir,
                                                    std::int64_t detect_timeout_s, std::string post_script) {
  ExternalMatch match;
  match.data_dir = game.data_dir;
  match.win_dir = win_dir;
  match.detect_timeout_s = detect_timeout_s;
  return TrackExternal(game, std::move(match), std::move(post_script));
}

Result<void> ProcessSupervisor::TrackExternal(const model::Game& game, ExternalMatch match,
                                              std::string post_script) {
  {
    std::lock_guard lock(mutex_);
    if (running_.contains(game.id)) {
      return Err("already_running", std::format("\"{}\" is already running", game.id));
    }
    // Reserved immediately, before detection even starts, for the same
    // reason Launch() reserves it before its process even exists yet:
    // without this, firing /launch twice in quick succession for the same
    // game starts two independent detection watchers that could both
    // eventually find the same real process. 0 is never a real pid (see
    // Stop()'s guard below) so it's unambiguous as "not confirmed yet".
    running_[game.id] = 0;
    external_[game.id] = match;  // for Stop()/WatchExternal()'s kill escalation
    AdoptWatcher(game.id, std::thread(&ProcessSupervisor::WatchExternal, this, game.id, std::move(match),
                                      model::NowSeconds(), std::move(post_script)));
  }
  return {};
}

void ProcessSupervisor::WatchExternal(std::string game_id, ExternalMatch match, std::int64_t requested_at,
                                      std::string post_script) {
  std::set<pid_t> matched;
  std::int64_t started_at = 0;
  ProcessIndex index;  // after the first refresh, each tick only lists /proc

  // Detection phase: wait for the launch to actually produce a process.
  while (!stopping_.load(std::memory_order_relaxed) && matched.empty()) {
    index.Refresh();
    matched = MatchExternal(index, match);
    if (!matched.empty()) break;
    if (model::NowSeconds() - requested_at >= match.detect_timeout_s) {
      log::Warn("never detected a process for {} after {}s -- giving up", game_id, match.detect_timeout_s);
      std::lock_guard lock(mutex_);
      running_.erase(game_id);
      prefixes_.erase(game_id);
      external_.erase(game_id);
      return;
    }
    if (PollWaitStopping()) break;
  }
  if (stopping_.load(std::memory_order_relaxed)) return;  // daemon going away

  started_at = model::NowSeconds();
  {
    std::lock_guard lock(mutex_);
    running_[game_id] = *matched.begin();
  }
  auto stamped = games_.Update(game_id, [&](model::Game& stored) { stored.last_played_at = started_at; });
  if (!stamped) log::Error("failed to record launch time for {}: {}", game_id, stamped.error().message);
  log::Info("detected {} running ({} process(es))", game_id, matched.size());
  json running_event = stamped ? model::ToJson(*stamped) : json{{"id", game_id}};
  running_event["state"] = "running";
  events_.Publish("game.state", std::move(running_event));

  // Liveness phase: re-match every tick rather than just poll the pids
  // already found, since the process tree can reshape early on.
  std::int64_t credited = 0;
  while (!stopping_.load(std::memory_order_relaxed)) {
    index.Refresh();
    const std::set<pid_t> current = MatchExternal(index, match);
    if (current.empty() && !AnyAlive(matched)) break;
    if (!current.empty()) matched = current;

    // Same SIGKILL escalation Watch() does for a directly-launched game;
    // missing here before meant Stop() on a Steam-launched game only ever
    // sent one SIGTERM and never followed up, so a game that ignored it kept
    // running forever with mirad unable to tell.
    {
      std::lock_guard lock(mutex_);
      const auto deadline = kill_deadlines_.find(game_id);
      if (deadline != kill_deadlines_.end() && model::NowSeconds() >= deadline->second) {
        log::Warn("{} (launched externally) ignored SIGTERM; sending SIGKILL", game_id);
        for (pid_t found : FindExternal(match)) ::kill(found, SIGKILL);
        kill_deadlines_.erase(deadline);
      }
    }

    const std::int64_t elapsed = model::NowSeconds() - started_at;
    if (elapsed - credited >= kCheckpointSeconds) {
      const std::int64_t delta = elapsed - credited;
      auto checkpoint = games_.Update(game_id, [&](model::Game& game) { game.play_seconds += delta; });
      if (checkpoint) credited = elapsed;
    }
    if (PollWaitStopping()) break;
  }
  if (stopping_.load(std::memory_order_relaxed)) return;  // daemon going away

  const std::int64_t ended_at = model::NowSeconds();
  const std::int64_t played = ended_at > started_at ? ended_at - started_at : 0;

  {
    std::lock_guard lock(mutex_);
    running_.erase(game_id);
    prefixes_.erase(game_id);
    external_.erase(game_id);
    kill_deadlines_.erase(game_id);
    stop_requested_.erase(game_id);
  }

  // No real exit code/signal available for a process Mira didn't spawn, so
  // no crash detection here -- Steam's own client already shows that;
  // last_error is left alone rather than guessed at.
  auto updated =
      games_.Update(game_id, [&](model::Game& game) { game.play_seconds += played - credited; });
  if (!updated) log::Error("failed to record playtime for {}: {}", game_id, updated.error().message);

  log::Info("{} (launched externally) exited after {}s", game_id, played);
  // Same reasoning as Watch()'s exit event: the full updated record rides
  // along so a listener can patch its one row instead of relisting.
  json event = updated ? model::ToJson(*updated) : json{{"id", game_id}};
  event["state"] = "exited";
  event["played_seconds"] = played;
  events_.Publish("game.state", std::move(event));

  RunScript(post_script, game_id, "post");
}

}  // namespace mira::proc
