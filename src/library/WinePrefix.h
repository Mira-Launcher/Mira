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

// The folder holding drive_c: `dir` itself, or its pfx/ when that has one (Steam's compatdata and
// umu's layout).
std::filesystem::path PrefixRoot(const std::filesystem::path& dir);

// The Windows user folder programs in the prefix write to: drive_c/users/steamuser once Proton
// has made it, else the one Wine names after $USER.
std::filesystem::path WindowsUserDir(const std::filesystem::path& prefix);

}  // namespace mira::library
