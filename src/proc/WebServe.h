#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mira::proc {

// mira-run's --serve: serves `dir` on 127.0.0.1:`port` while `argv` (a browser opening the game)
// runs, and returns its exit code, or 1 when the port can't be had. The headers let a game use
// SharedArrayBuffer (Godot's web exports need it) without blocking what it loads from elsewhere.
int ServeWhileRunning(const std::filesystem::path& dir, int port,
                      const std::vector<std::string>& argv);

}  // namespace mira::proc
