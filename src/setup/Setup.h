#pragma once

#include <filesystem>
#include <vector>

#include "core/Result.h"

namespace mira::setup {

// Where `mira setup` writes its user-level integration files.
struct SetupPaths {
  std::filesystem::path bin_dir;           // ~/.local/bin
  std::filesystem::path applications_dir;  // $XDG_DATA_HOME/applications
  std::filesystem::path icons_dir;         // $XDG_DATA_HOME/icons
};

SetupPaths DefaultPaths();

// Writes a `mira` wrapper (with a `mira-run` symlink) that runs the
// binaries bundled in `appimage`, a desktop entry and the icon. Refuses to replace a `mira` in bin_dir it didn't write.
// Returns the paths written.
Result<std::vector<std::filesystem::path>> Install(const SetupPaths& paths, const std::filesystem::path& appimage,
                                                   const std::filesystem::path& icon);

// Removes what Install wrote. Returns the paths removed.
Result<std::vector<std::filesystem::path>> Remove(const SetupPaths& paths);

}  // namespace mira::setup
