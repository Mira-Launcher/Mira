#pragma once

#include <cstdint>
#include <filesystem>
#include <json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "config/Config.h"
#include "model/Types.h"

namespace mira::saves {

// The folders a game's save locations are written against, each with a logical name. A found file
// is kept as one of these names plus a path inside it, never as an absolute path, so a backup
// restores to the right place after the game or its prefix moved, or its runner changed between
// Wine (users/$USER) and Proton (users/steamuser).
struct Roots {
  std::filesystem::path install;        // "install"
  std::filesystem::path prefix;         // "prefix": the folder holding drive_c; empty when native
  std::filesystem::path prefix_user;    // "prefix_user": drive_c/users/<name>
  std::filesystem::path steam_library;  // "steam_library": the Steam library holding the game
  std::filesystem::path steam;          // "steam": Steam's own folder (userdata)
  std::filesystem::path home;           // "home"
  std::filesystem::path xdg_data;
  std::filesystem::path xdg_config;
  bool windows = false;       // runs under Wine or Proton: Windows locations, folded case
  std::string store;          // "steam", "gog" or "epic"; empty when the store isn't known
  std::string store_game_id;  // its id there
  std::string store_user_id;  // the signed-in account's id there (Epic's {epicid}); may be empty
  std::string user_name;      // <osUserName>
};
Roots RootsFor(const config::Config& config, const model::Game& game);

// The absolute folder a logical root names; empty when this game has none.
std::filesystem::path RootPath(const Roots& roots, std::string_view root);

struct SaveFile {
  std::string root;  // a logical root
  std::string path;  // inside it, '/'-separated
  std::filesystem::path absolute;
  std::uintmax_t bytes = 0;
  std::int64_t mtime_ns = 0;  // since the Unix epoch
};

// A file time as nanoseconds since the Unix epoch, and back.
std::int64_t ToUnixNs(std::filesystem::file_time_type time);
std::filesystem::file_time_type FromUnixNs(std::int64_t ns);

struct Location {
  std::string pattern;  // as written in the list or in saves.extra_paths
  std::string path{};   // with this game's folders filled in; empty when it can't be
  // Why nothing was looked for: "unknown_placeholder" (a folder this game doesn't have, or one Mira
  // doesn't know), "too_broad" (it would take a whole prefix, home or user folder). Else empty.
  std::string problem{};
  std::vector<SaveFile> files{};
  std::vector<std::string> skipped{};  // symlinks inside a save folder, root:path, not followed
};

// `location` written in any of the three forms (the list's <winAppData>, GOG's
// <?APPLICATION_DATA_LOCAL?>, Epic's {appdata}) as absolute glob patterns, one per spelling of
// the folder (Wine's older "Application Data" and "My Documents" too). Empty when it uses a
// placeholder this game has no folder for.
std::vector<std::string> Expand(std::string_view location, const Roots& roots);

// A folder's path as a glob matching only itself: `*`, `?` and `[` bracketed.
std::string Escape(const std::filesystem::path& path);

// An absolute `path` (a glob pattern, folders escaped) written against the game's own folders where
// it lies in one, so it follows the game when it moves: "<base>/..." in its install folder,
// "<home>/..." in a Windows game's prefix user folder or a native game's home folder,
// "<prefix>/..." elsewhere in its prefix. The deepest folder wins; anything else is returned as it
// is.
std::string Shorten(std::string_view path, const Roots& roots);

// The files at `location`. A folder that matches brings every file under it. Symlinks are
// followed while matching the location's own folders, never inside a save folder, and Wine's
// dosdevices is never entered. Files outside every logical root are left out.
Location Find(std::string_view location, const Roots& roots);

// The file locations of a list entry ({files, registry, cloud}) that apply to this game, by their
// `when`: its system (Wine and Proton count as windows), and its store when that's known.
std::vector<std::string> FileLocations(const nlohmann::json& entry, const Roots& roots);

// Matches one path component against one glob component: '*', '?' and '[...]' ('[!...]' negated,
// "[[]" a literal '['), ASCII case folded when `fold`.
bool MatchComponent(std::string_view pattern, std::string_view name, bool fold);

}  // namespace mira::saves
