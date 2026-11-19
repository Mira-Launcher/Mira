#pragma once

#include <sys/types.h>

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <set>
#include <thread>

namespace mira::gamemode {

// Whether gamemoded/gamemoderun is on PATH -- distinct from IsDaemonRunning,
// so a caller can tell "not installed" apart from "installed but not
// running".
bool IsInstalled();

// Whether com.feralinteractive.GameMode currently owns its name on the
// session bus, i.e. gamemoded is actually reachable -- via the
// desktop-bus-standard NameHasOwner call, not anything GameMode-specific.
bool IsDaemonRunning();

// RegisterGame(pid)/UnregisterGame(pid) over GameMode's D-Bus interface, not
// a linked libgamemode (see GameMode.cpp). Both are best-effort and never
// block or fail a launch -- a missing daemon just means nothing happens.
void RegisterGame(pid_t pid);
void UnregisterGame(pid_t pid);

// Keeps every process in a game's cgroup registered while it runs, as gamemoderun's preload does:
// a launcher or updater that hands the game to a new process would otherwise leave none. What's in
// the group at the start registers at once, a later process once it has lived through one scan, so
// short-lived helpers are skipped. A process that leaves the group is unregistered.
class GroupClients {
 public:
  explicit GroupClients(std::filesystem::path group);
  ~GroupClients();  // stops scanning and unregisters what's still registered
  GroupClients(const GroupClients&) = delete;
  GroupClients& operator=(const GroupClients&) = delete;

 private:
  void Scan(bool settle);

  std::filesystem::path group_;
  std::set<pid_t> registered_;
  std::set<pid_t> seen_;  // found by the last scan, not registered yet
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::thread thread_;
};

}  // namespace mira::gamemode
