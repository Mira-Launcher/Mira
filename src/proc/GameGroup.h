#pragma once

#include <sys/types.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/Result.h"

namespace mira::proc {

// Every process of one game launch, whatever it forks or hands off to. On Linux that's a cgroup in
// mira-run's systemd scope. On macOS it's every process whose environment carries the launch's
// tag, which they all inherit, even ones that leave the process tree (wineserver, launchers).
class GameGroup {
 public:
  // mira-run's group for its game. Fails without a systemd user session on Linux.
  static Result<GameGroup> Create();
  // The group mira-run `wrapper` made, or nullopt once it's gone.
  static std::optional<GameGroup> Of(pid_t wrapper);

  // What the game's environment gets, to join on macOS. Empty on Linux.
  std::vector<std::string> Env() const;
  // Puts the calling process in the group, between fork and exec: async-signal-safe.
  bool JoinFromChild() const;

  std::vector<pid_t> Pids() const;
  bool Populated() const;
  // Blocks until the group is empty or `timeout` passes; true if it emptied.
  bool WaitEmpty(std::optional<std::chrono::milliseconds> timeout = {}) const;
  void Signal(int sig) const;
  void Kill() const;
  // Cleans up after the group once it's empty.
  void Remove() const;

 private:
#ifdef __linux__
  std::filesystem::path path_;
  std::string procs_;  // cgroup.procs, made ahead: the child can't allocate
#else
  pid_t wrapper_ = 0;
  std::string token_;  // mira-run's StartToken, so a reused pid isn't the same group
#endif
};

// The environment entry that tags processes on macOS: "MIRA_GROUP=<mira-run's StartToken>".
std::string GroupEntry(const std::string& id);

}  // namespace mira::proc
