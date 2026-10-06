#include "steam/FriendsStatus.h"

#include <signal.h>

#include <format>
#include <fstream>

#include "core/Command.h"
#include "core/Paths.h"
#include "runner/Exec.h"

namespace mira::steam {
namespace {

bool SteamRunning(const std::filesystem::path& pid_file) {
  std::ifstream file(pid_file);
  pid_t pid = 0;
  return file >> pid && pid > 0 && ::kill(pid, 0) == 0;
}

}  // namespace

std::filesystem::path DefaultPidFile() {
  return paths::Home() / ".steam" / "steam.pid";
}

Result<void> SetFriendsStatus(std::string_view status, const std::filesystem::path& pid_file) {
  if (status != "online" && status != "invisible") {
    return Err("invalid_status", std::format("unknown Steam status \"{}\"", status),
               "Use online or invisible.");
  }
  if (!SteamRunning(pid_file)) {
    return Err("steam_not_running", "Steam isn't running.",
               "Start Steam, then set the status again.");
  }
  Command command;
  command.argv = {"steam", std::format("steam://friends/status/{}", status)};
  if (auto spawned = runner::SpawnDetached(command); !spawned)
    return std::unexpected(spawned.error());
  return {};
}

}  // namespace mira::steam
