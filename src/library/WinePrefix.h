#pragma once

#include <filesystem>

namespace mira::library {

// True for a directory that is itself a Wine/Proton prefix (system.reg +
// drive_c, or a pfx/ subdirectory, umu's layout) rather than game content.
// Scanner excludes a prefix from being treated as a game folder; Detector
// must recheck this at every level of its recursive walk, since a nested
// prefix (e.g. umu's <root>/umu/umu-default/) would otherwise have its
// drive_c .exe files picked up as game candidates.
bool LooksLikeWinePrefix(const std::filesystem::path& dir);

// Inside a prefix, relative to it: Wine's own programs (drive_c/windows), not a game's.
bool IsPrefixSystemDir(const std::filesystem::path& rel);

}  // namespace mira::library
