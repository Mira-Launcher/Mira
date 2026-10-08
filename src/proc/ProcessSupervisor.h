#pragma once

#include <sys/types.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "api/EventBus.h"
#include "core/Command.h"
#include "core/Result.h"
#include "proc/Session.h"
#include "store/GameStore.h"

namespace mira::proc {

// Every pid whose WINEPREFIX/STEAM_COMPAT_DATA_PATH points at data_dir.
// Exposed for testing the prefix-collision rule (see the .cpp).
std::set<pid_t> FindPrefixProcesses(const std::string& data_dir);

// Every pid in data_dir's prefix whose command line starts under win_dir
// (lowercase with forward slashes, e.g. "c:/program files (x86)/hearthstone").
std::set<pid_t> FindDirProcesses(const std::string& data_dir, const std::string& win_dir);

// How a game Mira didn't spawn itself is found in /proc: by Steam appid, or
// by folder inside a store launcher's prefix.
struct ExternalMatch {
  std::string appid;
  std::string data_dir;
  std::string win_dir;
  std::int64_t detect_timeout_s = 60;
};

// Tracks the games currently running. One watcher thread per running game,
// fine at launcher scale, and unlike a blanket waitpid(-1) reaper it can't
// steal the exit status of runner::RunAndWait's own provisioning children.
//
// Polls with WNOHANG (not a blocking waitpid()) so shutdown doesn't wait on
// the player quitting; only ticks while a game is actually running.
class ProcessSupervisor {
public:
  ProcessSupervisor(store::GameStore& games, api::EventBus& events,
                    std::int64_t stop_timeout_s = 10);
  ~ProcessSupervisor();
  ProcessSupervisor(const ProcessSupervisor&) = delete;
  ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;

  // Starts the game and returns as soon as it's running. Publishes
  // game.state running now, and exited later, with playtime recorded.
  // post_script runs after the store/event are finalized. Fallback path
  // used only when mira-run couldn't be found or spawned (see api::Server);
  // the normal path is LaunchWrapped below.
  Result<void> Launch(const model::Game& game, const Command& command, std::string post_script = "");

  // The normal path: `wrapper_pid` is an already-running mira-run, spawned
  // by the caller after a successful "ok" on its status pipe (see
  // api::Server); `started_at` keys the session record it writes to mira.db
  // (proc::Session.h). pre/post_script aren't passed here -- mira-run owns
  // them, which is what makes them survive mirad dying mid-session.
  Result<void> LaunchWrapped(const model::Game& game, pid_t wrapper_pid, std::int64_t started_at);

  // Called once at mirad startup, before serving: closes out whatever a
  // previous mirad didn't get to see finish. A finished record is archived
  // immediately; a still-running mira-run is re-adopted (WatchReconciledLive)
  // so a relaunch can't duplicate it; anything else is closed out
  // `incomplete`. Works from the records not counted yet. Never fails outright.
  void Reconcile();

  // For a game Steam's own client launched (steam.launch_mode "steam"), which
  // Mira can't waitpid() on. Polls /proc for SteamAppId=<appid> or
  // SteamGameId=<appid> in a process's environment, since the actual game
  // sits under a steam -> reaper -> pressure-vessel -> proton chain with no
  // fixed pid. Reports running/exited like Launch(), minus a real exit
  // code/signal. Gives up quietly if nothing matches within a startup
  // window: Steam may still be launching, or the player cancelled.
  Result<void> TrackSteamLaunch(const model::Game& game, const std::string& appid,
                                std::string post_script = "");

  // Same, for a game a store launcher (Battle.net, Ubisoft, EA) starts.
  Result<void> TrackLauncherLaunch(const model::Game& game, const std::string& win_dir,
                                   std::int64_t detect_timeout_s, std::string post_script = "");

  // SIGTERM to the running game's whole process group, if any. Returns as
  // soon as the signal is sent; if the game ignores it, the watcher escalates
  // to SIGKILL after stop_timeout_s rather than blocking the caller.
  Result<void> Stop(const std::string& game_id);

  bool IsRunning(const std::string& game_id) const;

  // Claims a game before anything is spawned or any of its files change, so two quick
  // launches can't both start it and a game can't launch mid-move. Empty if it is
  // running or already claimed.
  class Reservation {
  public:
    Reservation(ProcessSupervisor& supervisor, std::string game_id)
        : supervisor_(&supervisor), game_id_(std::move(game_id)) {}
    Reservation(Reservation&& other) noexcept : supervisor_(std::exchange(other.supervisor_, nullptr)), game_id_(std::move(other.game_id_)) {}
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    ~Reservation() {
      if (supervisor_) supervisor_->Release(game_id_);
    }

  private:
    ProcessSupervisor* supervisor_;
    std::string game_id_;
  };
  // `purpose` says what holds it ("launched", "moved", "deleted"), for ReservedFor.
  std::optional<Reservation> Reserve(const std::string& game_id, std::string purpose = "launched");
  // What holds a game's claim, or "" if nothing does.
  std::string ReservedFor(const std::string& game_id) const;

  // Called with the game's id on its watcher thread after a Launch() or
  // LaunchWrapped() game exits. Set once, before any launch.
  void SetExitHook(std::function<void(const std::string& game_id)> hook) { exit_hook_ = std::move(hook); }

private:
  void Release(const std::string& game_id);
  // A watcher thread, and whether it has finished, so a replaced one can be joined as soon as it ends.
  struct Watcher {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  // Starts `body` on a thread watching `game_id`; mutex_ must be held.
  void AdoptWatcher(const std::string& game_id, std::function<void()> body);
  void Watch(std::string game_id, pid_t pid, std::int64_t started_at, std::string post_script);
  void WatchWrapped(std::string game_id, pid_t wrapper_pid, std::int64_t started_at);
  void WatchReconciledLive(std::string game_id, pid_t wrapper_pid, std::int64_t started_at);
  void FinalizeWrappedSession(const std::string& game_id, const proc::SessionRecord& record, bool requested_stop);
  // Takes a finished game off every running list; true if Stop() had asked it to quit.
  bool Forget(const std::string& game_id);
  Result<void> TrackExternal(const model::Game& game, ExternalMatch match, std::string post_script);
  void WatchExternal(std::string game_id, ExternalMatch match, std::int64_t requested_at,
                     std::string post_script);

  store::GameStore& games_;
  api::EventBus& events_;
  std::int64_t stop_timeout_s_;
  std::function<void(const std::string&)> exit_hook_;

  mutable std::mutex mutex_;
  std::map<std::string, pid_t> running_;
  std::map<std::string, std::string> prefixes_;  // game id -> data_dir, for Stop()
  std::map<std::string, ExternalMatch> external_;  // game id -> match, for Stop()/WatchExternal()
  std::map<std::string, std::int64_t> kill_deadlines_;  // game id -> when to SIGKILL
  std::map<std::string, std::string> reserved_;  // games claimed by Reserve(), with the purpose
  std::set<std::string> stop_requested_;  // Stop() was called; the exit isn't a crash
  std::map<std::string, Watcher> watchers_;
  // Watchers replaced by a relaunch of the same game; each is joined once it has finished.
  std::vector<Watcher> retired_;
  std::atomic<bool> stopping_{false};
  std::mutex stop_mutex_;
  std::condition_variable stop_wake_;  // ends a watcher's poll wait as soon as stopping_ is set

  // Waits one poll interval; true once the supervisor is shutting down.
  bool PollWaitStopping();
};

}  // namespace mira::proc
