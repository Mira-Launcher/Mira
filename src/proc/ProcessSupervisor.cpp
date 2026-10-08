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
#include "proc/ExitReason.h"
#include "proc/Cgroup.h"
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

// A game.state's exit fields, and for a crash its code, hint and a fix that opens the log.
void AddExit(json& event, const ExitOutcome& outcome, const ExitInfo& info, const std::string& game_id, bool windows) {
  event["state"] = outcome.crashed ? "crashed" : "exited";
  event["exit_code"] = info.exit_code;
  event["signal"] = info.signal;
  event["played_seconds"] = info.played_seconds;
  event["error"] = outcome.error;
  if (!outcome.crashed) return;
  event["code"] = outcome.code;
  if (outcome.code == "killed") {
    event["hint"] = "Closing other programs before playing leaves the game more memory.";
  } else if (outcome.code == "start_failed") {
    event["hint"] = "The game's log says what couldn't be found.";
  } else {
    event["hint"] = windows ? "The game's log usually says why. Another runner in the game's settings often helps."
                            : "The game's log usually says why.";
  }
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


// The cgroup mira-run put the game in (its scope's "game" child); nullopt once mira-run is gone.
std::optional<std::filesystem::path> WrappedGroup(pid_t wrapper_pid) {
  const auto scope = cgroup::Of(wrapper_pid);
  if (!scope) return std::nullopt;
  std::filesystem::path group = cgroup::GameGroup(*scope);
  std::error_code ec;
  if (!std::filesystem::is_directory(group, ec)) return std::nullopt;  // not a mira-run scope (gone, or a reused pid)
  return group;
}

bool AnyAlive(const std::set<pid_t>& pids) {
  for (pid_t pid : pids) {
    if (::kill(pid, 0) == 0) return true;
  }
  return false;
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
                         ? InPrefix(info.prefix, match.data_dir) && UnderWindowsPath(info.argv0, match.win_dir)
                         : info.steam_launch == match.appid;
    if (hit) found.insert(pid);
  }
  return found;
}

}  // namespace

// Every pid whose WINEPREFIX/STEAM_COMPAT_DATA_PATH points at data_dir.
//
// Finds a launcher's processes in a prefix Mira didn't start them in, where no cgroup
// of Mira's holds them.

std::set<pid_t> FindPrefixProcesses(const std::string& data_dir) {
  std::set<pid_t> found;
  if (data_dir.empty()) return found;
  ProcessIndex index;
  index.Refresh();
  for (const auto& [pid, info] : index.Processes()) {
    if (InPrefix(info.prefix, data_dir)) found.insert(pid);
  }
  return found;
}

std::set<pid_t> FindDirProcesses(const std::string& data_dir, const std::string& win_dir) {
  std::set<pid_t> found;
  if (data_dir.empty() || win_dir.empty()) return found;
  ProcessIndex index;
  index.Refresh();
  for (const auto& [pid, info] : index.Processes()) {
    if (InPrefix(info.prefix, data_dir) && UnderWindowsPath(info.argv0, win_dir)) found.insert(pid);
  }
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
  std::map<std::string, Watcher> watchers;
  std::vector<Watcher> retired;
  {
    std::lock_guard lock(mutex_);
    watchers.swap(watchers_);
    retired.swap(retired_);
  }
  for (auto& [id, watcher] : watchers) {
    if (watcher.thread.joinable()) watcher.thread.join();
  }
  for (Watcher& watcher : retired) {
    if (watcher.thread.joinable()) watcher.thread.join();
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

void ProcessSupervisor::AdoptWatcher(const std::string& game_id, std::function<void()> body) {
  // Ones replaced earlier that have ended cost a join that returns at once.
  std::erase_if(retired_, [](Watcher& watcher) {
    if (!watcher.done->load()) return false;
    if (watcher.thread.joinable()) watcher.thread.join();
    return true;
  });
  // The previous watcher for this id has erased itself from running_, but may
  // still be recording its exit, so it is kept to be joined, not detached.
  if (auto stale = watchers_.find(game_id); stale != watchers_.end()) {
    if (stale->second.thread.joinable()) retired_.push_back(std::move(stale->second));
    watchers_.erase(stale);
  }
  auto done = std::make_shared<std::atomic<bool>>(false);
  std::thread thread([body = std::move(body), done] {
    body();
    done->store(true);
  });
  watchers_[game_id] = Watcher{std::move(thread), std::move(done)};
}

Result<void> ProcessSupervisor::LaunchWrapped(const model::Game& game, pid_t wrapper_pid,
                                              std::int64_t session_started_at) {
  {
    std::lock_guard lock(mutex_);
    if (running_.contains(game.id)) {
      return Err("already_running", std::format("\"{}\" is already running", game.id));
    }
    running_[game.id] = wrapper_pid;
    AdoptWatcher(game.id, [this, id = game.id, wrapper_pid, session_started_at] {
      WatchWrapped(id, wrapper_pid, session_started_at);
    });
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
  std::optional<ExternalMatch> match;
  {
    std::lock_guard lock(mutex_);
    const auto it = running_.find(game_id);
    if (it == running_.end()) {
      return Err("not_running", std::format("\"{}\" is not running", game_id));
    }
    pid = it->second;
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
  if (match) {
    // Group (see runner::SpawnDetached's setpgid note) plus every pid FindSteamProcesses
    // finds under a Steam-launched game's appid: that tree is Steam's own
    // (reaper/pressure-vessel/proton/the game), not a child of mirad and not one shared
    // process group, so the group signal above only ever reaches whichever single pid
    // WatchExternal recorded.
    const std::set<pid_t> in_steam_tree = FindExternal(*match);
    bool signalled = ::kill(-pid, SIGTERM) == 0 || ::kill(pid, SIGTERM) == 0;
    for (pid_t found : in_steam_tree) { ::kill(found, SIGTERM); signalled = true; }
    if (!signalled) {
      return Err("stop_failed", std::format("could not signal pid {}", pid));
    }
  } else {
    // A mira-run game: everything it started is in its cgroup. No cgroup means mira-run is gone.
    const auto group = WrappedGroup(pid);
    if (!group) return Err("not_running", std::format("\"{}\" is not running", game_id));
    cgroup::Signal(*group, SIGTERM);
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

std::optional<ProcessSupervisor::Reservation> ProcessSupervisor::Reserve(const std::string& game_id,
                                                                         std::string purpose) {
  {
    std::lock_guard lock(mutex_);
    if (running_.contains(game_id) || !reserved_.emplace(game_id, std::move(purpose)).second) {
      return std::nullopt;
    }
  }
  return Reservation(*this, game_id);
}

std::string ProcessSupervisor::ReservedFor(const std::string& game_id) const {
  std::lock_guard lock(mutex_);
  const auto found = reserved_.find(game_id);
  return found != reserved_.end() ? found->second : std::string();
}

void ProcessSupervisor::Release(const std::string& game_id) {
  std::lock_guard lock(mutex_);
  reserved_.erase(game_id);
}

bool ProcessSupervisor::IsRunning(const std::string& game_id) const {
  std::lock_guard lock(mutex_);
  return running_.contains(game_id);
}

// Stop()'s SIGKILL escalation, for WatchWrapped and WatchReconciledLive: a game that
// ignored SIGTERM past its deadline gets SIGKILL to its whole cgroup.
void ProcessSupervisor::KillIfOverdue(const std::string& game_id, pid_t wrapper_pid) {
  std::lock_guard lock(mutex_);
  const auto deadline = kill_deadlines_.find(game_id);
  if (deadline == kill_deadlines_.end() || model::NowSeconds() < deadline->second) return;
  log::Warn("{} ignored SIGTERM; sending SIGKILL", game_id);
  if (const auto group = WrappedGroup(wrapper_pid)) cgroup::Kill(*group);
  kill_deadlines_.erase(deadline);
}

void ProcessSupervisor::WatchWrapped(std::string game_id, pid_t wrapper_pid, std::int64_t started_at) {
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

    KillIfOverdue(game_id, wrapper_pid);
    if (PollWaitStopping()) break;
  }
  if (stopping_.load(std::memory_order_relaxed)) return;  // daemon going away

  auto record = ReadSessionRecord(games_.File(), game_id, started_at);
  const bool requested_stop = Forget(game_id);
  if (!record) {
    // mira-run vanished without writing a record (killed before it could
    // fork, or before its first write) -- writes are temp-file-then-rename,
    // so a reader never sees a partial file.
    log::Warn("mira-run for {} exited with no session record ({})", game_id, record.error().message);
    return;
  }
  CloseOutUnfinished(*record);
  FinalizeWrappedSession(game_id, *record, requested_stop);
}

bool ProcessSupervisor::Forget(const std::string& game_id) {
  std::lock_guard lock(mutex_);
  running_.erase(game_id);
  external_.erase(game_id);
  kill_deadlines_.erase(game_id);
  return stop_requested_.erase(game_id) > 0;
}

// Shared by WatchWrapped and Reconcile/WatchReconciledLive: classify the
// record, update the store (which marks it counted), publish the event.
void ProcessSupervisor::FinalizeWrappedSession(const std::string& game_id, const proc::SessionRecord& record,
                                               bool requested_stop) {
  const ExitInfo info{.exit_code = record.exit_code,
                      .signal = record.signal,
                      .requested_stop = requested_stop,
                      .launch_error = record.launch_error,
                      .played_seconds = record.duration_seconds,
                      .log_tail = ReadLogTail(GameLogPath(games_.Dir(), game_id))};
  // A record with no exit info at all (mira-run itself SIGKILLed before
  // finishing) is left un-crashed rather than guessed at.
  const ExitOutcome outcome =
      record.incomplete
          ? ExitOutcome{.crashed = false, .code = {}, .error = "Mira restarted during this session, so how it ended is unknown"}
                        : ClassifyExit(info);

  const store::PlaySession session{.game_id = game_id,
                                   .started_at = record.started_at,
                                   .ended_at = record.ended_at,
                                   .duration_seconds = record.duration_seconds,
                                   .exit_code = record.exit_code,
                                   .signal = record.signal,
                                   .incomplete = record.incomplete};
  auto updated = games_.FinishSession(session, [&](model::Game& game) {
    // Counted once, even for a record that comes back uncounted after a crash.
    if (record.started_at > game.last_session_at) {
      game.play_seconds += record.duration_seconds;
      game.last_session_at = record.started_at;
    }
    game.last_error = outcome.error;
  });
  if (!updated) {
    log::Error("failed to record playtime for {}: {}", game_id, updated.error().message);
  }

  if (outcome.crashed) {
    log::Warn("{}: {}", game_id, outcome.error);
  } else {
    log::Info("{} exited (code {}, signal {}) after {}s", game_id, info.exit_code, info.signal, info.played_seconds);
  }

  json event = updated ? model::ToJson(*updated) : json{{"id", game_id}};
  AddExit(event, outcome, info, game_id, updated && updated->platform == model::Platform::Windows);
  events_.Publish("game.state", std::move(event));
  if (exit_hook_) exit_hook_(game_id);
}

void ProcessSupervisor::WatchReconciledLive(std::string game_id, pid_t wrapper_pid, std::int64_t started_at) {
  // Not this mirad's child, so waitpid() can't work -- poll liveness instead.
  while (!stopping_.load(std::memory_order_relaxed) && ::kill(wrapper_pid, 0) == 0) {
    KillIfOverdue(game_id, wrapper_pid);
    if (PollWaitStopping()) break;
  }
  if (stopping_.load(std::memory_order_relaxed)) return;  // daemon going away

  auto record = ReadSessionRecord(games_.File(), game_id, started_at);
  const bool requested_stop = Forget(game_id);
  if (!record) {
    log::Warn("re-adopted mira-run for {} exited with no readable session record ({})", game_id,
             record.error().message);
    return;
  }
  CloseOutUnfinished(*record);
  FinalizeWrappedSession(game_id, *record, requested_stop);
}

void ProcessSupervisor::Reconcile() {
  for (proc::SessionRecord& record : UncountedSessions(games_.File())) {
    if (record.finished) {
      // mira-run had already finished and written the final record, but
      // the mirad that was supposed to notice and count it died first.
      FinalizeWrappedSession(record.game_id, record, /*requested_stop=*/false);
      continue;
    }

    const bool wrapper_alive = record.wrapper_pid > 0 && ::kill(record.wrapper_pid, 0) == 0;
    if (wrapper_alive) {
      log::Info("re-adopting live session for {} (mira-run pid {})", record.game_id, record.wrapper_pid);
      std::lock_guard lock(mutex_);
      running_[record.game_id] = record.wrapper_pid;
      AdoptWatcher(record.game_id, [this, id = record.game_id, wrapper_pid = record.wrapper_pid,
                                    started_at = record.started_at] {
        WatchReconciledLive(id, wrapper_pid, started_at);
      });
    } else {
      log::Warn("session for {} was left behind by a mira-run (pid {}) that's no longer running; closing it out "
               "as incomplete",
               record.game_id, record.wrapper_pid);
      record.incomplete = true;
      CloseOutUnfinished(record);
      FinalizeWrappedSession(record.game_id, record, /*requested_stop=*/false);
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
    // reason LaunchWrapped() reserves it before its process even exists yet:
    // without this, firing /launch twice in quick succession for the same
    // game starts two independent detection watchers that could both
    // eventually find the same real process. 0 is never a real pid (see
    // Stop()'s guard below) so it's unambiguous as "not confirmed yet".
    running_[game.id] = 0;
    external_[game.id] = match;  // for Stop()/WatchExternal()'s kill escalation
    AdoptWatcher(game.id, [this, id = game.id, match = std::move(match), requested_at = model::NowSeconds(),
                           post_script = std::move(post_script)] {
      WatchExternal(id, match, requested_at, post_script);
    });
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
      Forget(game_id);
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

    // SIGKILL escalation, as KillIfOverdue does for a mira-run game: without it a Steam
    // game that ignored Stop's SIGTERM kept running with mirad unable to tell.
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

  Forget(game_id);

  // No real exit code/signal available for a process Mira didn't spawn, so
  // no crash detection here -- Steam's own client already shows that;
  // last_error is left alone rather than guessed at.
  auto updated =
      games_.Update(game_id, [&](model::Game& game) { game.play_seconds += played - credited; });
  if (!updated) log::Error("failed to record playtime for {}: {}", game_id, updated.error().message);

  log::Info("{} (launched externally) exited after {}s", game_id, played);
  // The full updated record rides along so a listener can patch its one row instead of relisting.
  json event = updated ? model::ToJson(*updated) : json{{"id", game_id}};
  event["state"] = "exited";
  event["played_seconds"] = played;
  events_.Publish("game.state", std::move(event));

  RunScript(post_script, game_id, "post");
}

}  // namespace mira::proc
