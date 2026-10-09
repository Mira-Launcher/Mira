#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/Result.h"

namespace mira::steam {

struct ShortcutChange {
  // Steam account folders under userdata/ whose shortcuts.vdf gained or changed the entry.
  std::vector<std::string> added;
  std::vector<std::string> updated;
};

// Makes sure every Steam account's shortcuts.vdf has a non-Steam shortcut
// named `name` running `exe` with `launch_options`, so Steam's Big Picture
// can switch to it. An entry by that name is updated, not duplicated; other
// entries are written back unchanged. Steam reads the file when it starts.
Result<ShortcutChange> EnsureShortcut(const std::filesystem::path& steam_root, const std::string& name,
                                      const std::string& exe, const std::string& launch_options);

}  // namespace mira::steam
