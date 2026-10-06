#pragma once

#include <sys/types.h>

#include <cstdint>
#include <istream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mira::proc {

// What game detection needs from a process, read once when it first shows
// up. A refresh then costs one /proc listing, not a read of every process.
struct ProcessInfo {
  std::string steam_launch;  // AppId of a Steam "reaper SteamLaunch" process
  std::string prefix;       // WINEPREFIX or STEAM_COMPAT_DATA_PATH
  std::string argv0;        // lowercase, forward slashes
  std::int64_t first_seen = 0;
};

class ProcessIndex {
public:
  // Drops exited processes and reads new ones. A process is re-read for a
  // few seconds after it appears, since it may still exec into the game.
  void Refresh();

  const std::unordered_map<pid_t, ProcessInfo>& Processes() const { return processes_; }

private:
  std::unordered_map<pid_t, ProcessInfo> processes_;
};

// For Steam's "reaper SteamLaunch AppId=<id> -- <game>" wrapper, which lives
// exactly as long as Steam considers the game running: the AppId. `rest` is
// the command line after argv0. Empty for any other process.
std::string SteamLaunchAppId(const std::string& argv0, std::istream& rest);

// `argv0` is `win_path` itself or a file under it (both lowercase, forward slashes).
bool UnderWindowsPath(std::string_view argv0, std::string_view win_path);

// `value` is data_dir or a path under it (umu uses "<data_dir>/pfx/").
bool InPrefix(std::string_view value, std::string_view data_dir);

}  // namespace mira::proc
