#pragma once

#include <sys/types.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

#include "core/Result.h"

namespace mira::proc::cgroup {

std::optional<std::filesystem::path> Of(pid_t pid);  // "/sys/fs/cgroup" + the path on the "0::" line of /proc/<pid>/cgroup; nullopt if absent (cgroup v1 or no such pid)
std::filesystem::path GameGroup(const std::filesystem::path& scope);  // scope / "game"
Result<void> Create(const std::filesystem::path& group);   // mkdir; ok if it exists
std::vector<pid_t> Pids(const std::filesystem::path& group);  // cgroup.procs
bool Populated(const std::filesystem::path& group);        // cgroup.events has "populated 1"; false if unreadable
bool WaitEmpty(const std::filesystem::path& group, std::optional<std::chrono::milliseconds> timeout = {});  // blocks until the group is empty or `timeout` passes; true if it emptied
void Signal(const std::filesystem::path& group, int sig);  // kill(pid, sig) each Pids()
void Kill(const std::filesystem::path& group);             // write "1" to group/cgroup.kill; if that fails, Signal(group, SIGKILL)

}  // namespace mira::proc::cgroup
