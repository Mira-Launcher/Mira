#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mira::paths {

// Everything the user might want to back up or put under version control lives
// in one directory: $XDG_CONFIG_HOME/mira (default ~/.config/mira). This is a
// deliberate departure from the usual XDG three-way config/data/cache split:
// backup-friendliness was asked for explicitly, and a single folder of TOML
// files serves that better than state scattered across three directories.
std::filesystem::path UserDir();

// The UDS socket is transient IPC, not user data, so it stays under
// XDG_RUNTIME_DIR rather than in UserDir().
std::filesystem::path RuntimeDir();

std::filesystem::path Home();

std::filesystem::path SettingsFile();  // <UserDir>/settings.toml
std::filesystem::path DatabaseFile();  // <UserDir>/mira.db

// Expands a leading "~" and any $VAR references, so config files can be
// written the way a user would naturally type a path.
std::filesystem::path Expand(std::string_view raw);

// Whether `target` resolves (symlinks included) inside one of `roots`. A root
// itself only counts with `allow_equal`. Empty roots never match.
bool IsWithin(const std::filesystem::path& target, const std::vector<std::filesystem::path>& roots,
              bool allow_equal = false);

// The entries directly under `dir`, or none when it can't be read. Unlike a
// range-for over directory_iterator this never throws, even if the folder
// changes or a node becomes unreadable mid-listing.
std::vector<std::filesystem::path> ListDir(const std::filesystem::path& dir);

}  // namespace mira::paths
