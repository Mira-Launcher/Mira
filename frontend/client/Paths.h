#pragma once

#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>

// src/core/Paths.cpp's folders, which the frontend can't link. Keep the two in step.
namespace mira_gui::paths {

inline std::filesystem::path EnvOr(const char* name, const std::filesystem::path& fallback) {
  const char* value = std::getenv(name);
  return value && *value ? std::filesystem::path(value) : fallback;
}

inline std::filesystem::path Home() {
  if (const char* home = std::getenv("HOME"); home && *home) return home;
  if (const passwd* pw = ::getpwuid(::getuid())) return pw->pw_dir;
  return "/tmp";
}

// Where the desktop keeps per-user settings: $XDG_CONFIG_HOME or ~/.config.
inline std::filesystem::path ConfigHome() {
  return EnvOr("XDG_CONFIG_HOME", Home() / ".config");
}

// Mira's own folder: settings.toml, mira.db, themes.
inline std::filesystem::path UserDir() {
#ifdef __APPLE__
  if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
    return std::filesystem::path(config) / "mira";
  return Home() / "Library" / "Application Support" / "Mira";
#else
  return ConfigHome() / "mira";
#endif
}

// Never /tmp, where another user could make mira/ first and take the socket's place.
inline std::filesystem::path RuntimeDir() {
#ifdef __APPLE__
  return EnvOr("XDG_RUNTIME_DIR",
               EnvOr("TMPDIR", EnvOr("XDG_CACHE_HOME", Home() / "Library" / "Caches")));
#else
  return EnvOr("XDG_RUNTIME_DIR", EnvOr("XDG_CACHE_HOME", Home() / ".cache"));
#endif
}

}  // namespace mira_gui::paths
