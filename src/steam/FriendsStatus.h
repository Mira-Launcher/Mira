#pragma once

#include <filesystem>
#include <string_view>

#include "core/Result.h"

namespace mira::steam {

// Where a running Steam client writes its pid: ~/.steam/steam.pid.
std::filesystem::path DefaultPidFile();

// Sets the Steam friends status ("online" or "invisible") through
// `steam steam://friends/status/<status>`. Refused while Steam isn't running
// (by `pid_file`), since the URL would start the client just to set a status.
Result<void> SetFriendsStatus(std::string_view status,
                              const std::filesystem::path& pid_file = DefaultPidFile());

}  // namespace mira::steam
