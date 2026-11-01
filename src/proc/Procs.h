#pragma once

#include <sys/types.h>

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

// The other processes Mira can see: /proc on Linux, libproc and sysctl on macOS.
namespace mira::proc {

std::vector<pid_t> AllPids();
// NUL-separated, as /proc/<pid>/environ and /proc/<pid>/cmdline give them. nullopt for a process
// that's gone or isn't the user's.
std::optional<std::string> Environ(pid_t pid);
std::optional<std::string> Cmdline(pid_t pid);
// The short process name (/proc/<pid>/comm), or "" when it's gone.
std::string Name(pid_t pid);
// Whether `environ` (as Environ() gives it) holds exactly this KEY=VALUE entry.
bool HasEntry(const std::string& environ, const std::string& entry);
// Every process whose environment holds `entry`.
std::vector<pid_t> WithEntry(const std::string& entry);
// Whether any process has one of these files (canonical paths) open for writing.
bool AnyOpenForWriting(const std::set<std::string>& files);
#ifdef __APPLE__
// The pid and when it started: unlike the pid alone, never another process's later.
std::string StartToken(pid_t pid);
#endif
// This program's own path.
std::filesystem::path SelfExe();

}  // namespace mira::proc
