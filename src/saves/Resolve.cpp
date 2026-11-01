#include "saves/Resolve.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

#include "core/Strings.h"
#include "epic/Legendary.h"
#include "library/WinePrefix.h"
#include "steam/SteamDetector.h"

namespace mira::saves {
namespace {
using nlohmann::json;
namespace fs = std::filesystem;

std::string EnvOr(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return value != nullptr && *value != '\0' ? std::string(value) : fallback;
}

bool HasWildcard(std::string_view component) {
  return component.find_first_of("*?[") != std::string_view::npos;
}

}  // namespace

std::string Escape(const fs::path& path) {
  std::string out;
  for (const char c : path.generic_string()) {
    if (c == '[' || c == '*' || c == '?')
      out += std::string("[") + c + "]";
    else
      out += c;
  }
  return out;
}

namespace {

std::vector<std::string> Components(std::string_view pattern) {
  std::vector<std::string> parts;
  for (const std::string& part : strings::Split(pattern, '/'))
    if (!part.empty() && part != ".") parts.push_back(part);
  return parts;
}

bool SameName(std::string_view a, std::string_view b, bool fold) {
  return fold ? strings::ToLower(a) == strings::ToLower(b) : a == b;
}

// The folders each placeholder stands for, by its lower-cased name, whichever form wrote it.
std::vector<fs::path> Folders(std::string_view name, const Roots& roots) {
  const fs::path& user = roots.prefix_user;
  const auto windows = [&](std::initializer_list<const char*> under) {
    std::vector<fs::path> folders;
    if (!roots.windows || user.empty()) return folders;
    for (const char* sub : under) folders.push_back(user / sub);
    return folders;
  };
  const auto in_prefix = [&](const char* sub) -> std::vector<fs::path> {
    if (!roots.windows || roots.prefix.empty()) return {};
    return {roots.prefix / "drive_c" / sub};
  };
  if (name == "base" || name == "install" || name == "installdir") {
    if (roots.install.empty()) return {};
    return {roots.install};
  }
  if (name == "prefix") {
    if (roots.prefix.empty()) return {};
    return {roots.prefix};
  }
  if (name == "root") {
    if (!roots.steam_library.empty() || !roots.steam.empty()) {
      std::vector<fs::path> folders;
      for (const fs::path& folder : {roots.steam_library, roots.steam})
        if (!folder.empty() && !std::ranges::contains(folders, folder)) folders.push_back(folder);
      return folders;
    }
    if (roots.install.empty()) return {};
    return {roots.install.parent_path()};
  }
  if (name == "home") {
    if (!roots.windows) return {roots.home};
    if (user.empty()) return {};
    return {user};
  }
  if (name == "winappdata" || name == "application_data_roaming" || name == "roaming")
    return windows({"AppData/Roaming", "Application Data"});
  if (name == "winlocalappdata" || name == "application_data_local" || name == "appdata")
    return windows({"AppData/Local", "Local Settings/Application Data"});
  if (name == "winlocalappdatalow" || name == "application_data_local_low" || name == "locallow")
    return windows({"AppData/LocalLow"});
  if (name == "windocuments" || name == "documents" || name == "userdir")
    return windows({"Documents", "My Documents"});
  if (name == "saved_games" || name == "usersavedgames") return windows({"Saved Games"});
  if (name == "winpublic") return in_prefix("users/Public");
  if (name == "winprogramdata") return in_prefix("ProgramData");
  if (name == "windir") return in_prefix("windows");
  if (name == "xdgdata" && !roots.windows) return {roots.xdg_data};
  if (name == "xdgconfig" && !roots.windows) return {roots.xdg_config};
  return {};
}

// What a placeholder stands for as glob text: folders escaped, a store user id as any name.
std::vector<std::string> Values(std::string_view name, const Roots& roots) {
  if (name == "storeuserid") return {"*"};
  if (name == "epicid") return {roots.store_user_id.empty() ? "*" : Escape(roots.store_user_id)};
  if (name == "osusername") return {Escape(roots.user_name)};
  if (name == "game") return {Escape(roots.install.filename())};
  if (name == "storegameid") {
    if (roots.store_game_id.empty()) return {};
    return {Escape(roots.store_game_id)};
  }
  std::vector<std::string> values;
  for (const fs::path& folder : Folders(name, roots))
    values.push_back(Escape(folder.lexically_normal()));
  return values;
}

// The folders no location may take whole: a prefix, a home or user folder, or one of their
// shared folders, which hold every other program's files too.
std::vector<fs::path> Protected(const Roots& roots) {
  std::vector<fs::path> dirs = {"/",
                                roots.home,
                                roots.home / ".local",
                                roots.xdg_data,
                                roots.xdg_config,
                                roots.home / "Documents"};
  if (!roots.prefix.empty()) {
    for (const char* sub : {"", "drive_c", "drive_c/users", "drive_c/users/Public",
                            "drive_c/ProgramData", "drive_c/windows"})
      dirs.push_back(roots.prefix / sub);
  }
  if (!roots.prefix_user.empty()) {
    for (const char* sub :
         {"", "AppData", "AppData/Roaming", "AppData/Local", "AppData/LocalLow", "Application Data",
          "Local Settings", "Documents", "My Documents", "Saved Games"})
      dirs.push_back(roots.prefix_user / sub);
  }
  if (!roots.steam.empty()) dirs.insert(dirs.end(), {roots.steam, roots.steam / "userdata"});
  if (!roots.steam_library.empty()) dirs.push_back(roots.steam_library);
  for (fs::path& dir : dirs) dir = dir.lexically_normal();
  return dirs;
}

bool TooBroad(const std::string& pattern, const Roots& roots) {
  const std::vector<std::string> parts = Components(pattern);
  fs::path fixed = "/";
  std::size_t at = 0;
  for (; at < parts.size() && !HasWildcard(parts[at]); ++at) fixed /= parts[at];
  bool only_wildcards = true;
  bool recursive = false;
  for (std::size_t rest = at; rest < parts.size(); ++rest) {
    if (parts[rest] == "**") recursive = true;
    if (parts[rest] != "*" && parts[rest] != "**") only_wildcards = false;
  }
  if (!only_wildcards && !recursive) return false;
  // Also through symlinks: Wine links a prefix's Documents to the home folder's.
  std::error_code ec;
  const fs::path real = fs::weakly_canonical(fixed, ec);
  for (const fs::path& dir : Protected(roots)) {
    if (dir.empty()) continue;
    if (SameName(fixed.lexically_normal().generic_string(), dir.generic_string(), roots.windows))
      return true;
    std::error_code dir_ec;
    if (!ec && real == fs::weakly_canonical(dir, dir_ec) && !dir_ec) return true;
  }
  return false;
}

// Walks one pattern's components from "/", collecting what it matches.
struct Walker {
  bool fold;
  std::vector<std::string> parts;
  std::set<fs::path> files{};
  std::set<fs::path> skipped{};

  void Step(const fs::path& at, std::size_t index) {
    if (index == parts.size()) return Collect(at);
    const std::string& part = parts[index];
    std::error_code ec;
    if (part == "**") {
      Step(at, index + 1);
      for (const auto& entry : fs::directory_iterator(at, ec)) {
        if (entry.is_symlink(ec) || !entry.is_directory(ec) || IsDosDevices(entry.path())) continue;
        Step(entry.path(), index);
      }
      return;
    }
    if (!HasWildcard(part) && !fold) {
      if (fs::exists(at / part, ec)) Step(at / part, index + 1);
      return;
    }
    // A plain name is still looked up in the listing: on a case-insensitive disk (macOS) exists()
    // matches any case and would keep the pattern's spelling instead of the file's. An exact match
    // wins over other cases, and exists() still covers what no listing shows (.., unreadable
    // folders).
    const bool plain = !HasWildcard(part);
    if (plain) {
      for (const auto& entry : fs::directory_iterator(at, ec)) {
        if (entry.path().filename() == part) return Step(entry.path(), index + 1);
      }
    }
    bool matched = false;
    for (const auto& entry : fs::directory_iterator(at, ec)) {
      const std::string name = entry.path().filename().string();
      if (IsDosDevices(entry.path())) continue;
      if (MatchComponent(part, name, fold)) {
        matched = true;
        Step(entry.path(), index + 1);
      }
    }
    if (plain && !matched && fs::exists(at / part, ec)) Step(at / part, index + 1);
  }

  // A matched path: the file, or every file under the folder without following its symlinks.
  void Collect(const fs::path& path) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
      files.insert(path);
      return;
    }
    if (!fs::is_directory(path, ec)) return;
    for (auto it = fs::recursive_directory_iterator(
             path, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (it->is_symlink(ec)) {
        skipped.insert(it->path());
        it.disable_recursion_pending();
      } else if (it->is_regular_file(ec)) {
        files.insert(it->path());
      }
    }
  }

  static bool IsDosDevices(const fs::path& path) {
    return strings::ToLower(path.filename().string()) == "dosdevices";
  }
};

// The logical root holding `path` (the deepest one) and the path inside it.
std::optional<std::pair<std::string, std::string>> Logical(const fs::path& path,
                                                           const Roots& roots) {
  std::optional<std::pair<std::string, std::string>> best;
  std::size_t depth = 0;
  for (const char* name : {"install", "prefix", "prefix_user", "steam_library", "steam", "home"}) {
    const fs::path root = RootPath(roots, name).lexically_normal();
    if (root.empty() || root == "/") continue;
    const fs::path relative = path.lexically_normal().lexically_relative(root);
    if (relative.empty() || *relative.begin() == "..") continue;
    const std::size_t parts = std::distance(root.begin(), root.end());
    if (!best || parts > depth) {
      best = {{name, relative.generic_string()}};
      depth = parts;
    }
  }
  return best;
}

}  // namespace

std::int64_t ToUnixNs(std::filesystem::file_time_type time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::file_clock::to_sys(time).time_since_epoch())
      .count();
}

std::filesystem::file_time_type FromUnixNs(std::int64_t ns) {
  return std::chrono::file_clock::from_sys(
      std::chrono::sys_time<std::chrono::nanoseconds>(std::chrono::nanoseconds(ns)));
}

Roots RootsFor(const config::Config& config, const model::Game& game) {
  Roots roots;
  roots.install = fs::path(game.install_path).lexically_normal();
  roots.home = EnvOr("HOME", "/");
  roots.xdg_data = EnvOr("XDG_DATA_HOME", (roots.home / ".local/share").string());
  roots.xdg_config = EnvOr("XDG_CONFIG_HOME", (roots.home / ".config").string());
  roots.windows = game.platform == model::Platform::Windows;
  roots.user_name = EnvOr("USER", "user");
  if (roots.windows && !game.data_dir.empty()) {
    roots.prefix = library::PrefixRoot(game.data_dir);
    roots.prefix_user = library::WindowsUserDir(roots.prefix);
    roots.user_name = roots.prefix_user.filename().string();
  }
  if (game.source == "steam" || game.source == "gog" || game.source == "epic") {
    roots.store = game.source;
    roots.store_game_id = game.source_ref;
  }
  if (game.source == "epic") roots.store_user_id = epic::AccountId();
  if (game.runner_ref.starts_with("steam:")) {
    roots.store = "steam";
    roots.store_game_id = game.runner_ref.substr(std::string_view("steam:").size());
    // <library>/steamapps/common/<game>
    if (roots.install.parent_path().parent_path().filename() == "steamapps")
      roots.steam_library = roots.install.parent_path().parent_path().parent_path();
    if (const auto steam = steam::FindSteamRoot(config)) roots.steam = steam->lexically_normal();
  }
  return roots;
}

fs::path RootPath(const Roots& roots, std::string_view root) {
  if (root == "install") return roots.install;
  if (root == "prefix") return roots.prefix;
  if (root == "prefix_user") return roots.prefix_user;
  if (root == "steam_library") return roots.steam_library;
  if (root == "steam") return roots.steam;
  if (root == "home") return roots.home;
  return {};
}

bool MatchComponent(std::string_view pattern, std::string_view name, bool fold) {
  const auto same = [fold](char a, char b) {
    return fold ? std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b))
                : a == b;
  };
  // At pattern[at] == '[': whether `c` is in the class, and where the class ends. nullopt when
  // there's no closing ']', so the '[' is literal.
  const auto in_class = [&](std::size_t at, char c) -> std::optional<std::pair<bool, std::size_t>> {
    std::size_t i = at + 1;
    const bool negated = i < pattern.size() && pattern[i] == '!';
    if (negated) ++i;
    bool found = false;
    for (bool first = true; i < pattern.size(); first = false) {
      if (pattern[i] == ']' && !first) return std::pair{found != negated, i + 1};
      if (i + 2 < pattern.size() && pattern[i + 1] == '-' && pattern[i + 2] != ']') {
        const auto lower = [&](char x) {
          return fold ? static_cast<char>(std::tolower(static_cast<unsigned char>(x))) : x;
        };
        if (lower(c) >= lower(pattern[i]) && lower(c) <= lower(pattern[i + 2])) found = true;
        i += 3;
      } else {
        if (same(pattern[i], c)) found = true;
        ++i;
      }
    }
    return std::nullopt;
  };

  std::size_t p = 0;
  std::size_t n = 0;
  std::size_t star = std::string_view::npos;
  std::size_t resume = 0;
  while (n < name.size()) {
    bool advanced = false;
    if (p < pattern.size()) {
      if (pattern[p] == '*') {
        star = p++;
        resume = n;
        continue;
      }
      if (pattern[p] == '?') {
        advanced = true;
        ++p;
      } else if (pattern[p] == '[') {
        if (const auto cls = in_class(p, name[n])) {
          if (cls->first) {
            advanced = true;
            p = cls->second;
          }
        } else if (same('[', name[n])) {
          advanced = true;
          ++p;
        }
      } else if (same(pattern[p], name[n])) {
        advanced = true;
        ++p;
      }
    }
    if (advanced) {
      ++n;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      n = ++resume;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

std::vector<std::string> Expand(std::string_view location, const Roots& roots) {
  std::string text(location);
  std::ranges::replace(text, '\\', '/');
  std::vector<std::string> out = {""};
  std::size_t at = 0;
  while (at < text.size()) {
    std::size_t close = std::string::npos;
    std::size_t skip = 0;
    if (text.compare(at, 2, "<?") == 0) {
      close = text.find("?>", at + 2);
      skip = 2;
    } else if (text[at] == '<') {
      close = text.find('>', at + 1);
      skip = 1;
    } else if (text[at] == '{') {
      close = text.find('}', at + 1);
      skip = 1;
    }
    if (close == std::string::npos) {
      for (std::string& pattern : out) pattern += text[at];
      ++at;
      continue;
    }
    const std::string name = strings::ToLower(text.substr(at + skip, close - at - skip));
    const std::vector<std::string> values = Values(name, roots);
    if (values.empty()) return {};
    std::vector<std::string> grown;
    for (const std::string& pattern : out)
      for (const std::string& value : values) grown.push_back(pattern + value);
    out = std::move(grown);
    at = close + (skip == 2 ? 2 : 1);
  }
  // Only absolute patterns: a relative one would be read against mirad's own folder.
  std::erase_if(out, [](const std::string& pattern) { return !pattern.starts_with('/'); });
  return out;
}

std::string Shorten(std::string_view path, const Roots& roots) {
  std::vector<std::pair<fs::path, std::string_view>> candidates = {{roots.install, "<base>"}};
  if (roots.windows) {
    candidates.emplace_back(roots.prefix_user, "<home>");
    candidates.emplace_back(roots.prefix, "<prefix>");
  } else {
    candidates.emplace_back(roots.home, "<home>");
  }
  std::string best(path);
  std::size_t deepest = 0;
  for (const auto& [root, name] : candidates) {
    if (root.empty()) continue;
    // A pattern spells the folder escaped, as Values fills a placeholder in.
    std::string written = Escape(root.lexically_normal());
    while (written.size() > 1 && written.ends_with('/')) written.pop_back();
    if (written.size() <= deepest || !path.starts_with(written) ||
        (path.size() > written.size() && path[written.size()] != '/'))
      continue;
    deepest = written.size();
    best = std::string(name) + std::string(path.substr(written.size()));
  }
  return best;
}

Location Find(std::string_view location, const Roots& roots) {
  Location found{.pattern = std::string(location)};
  const std::vector<std::string> patterns = Expand(location, roots);
  if (patterns.empty()) {
    found.problem = "unknown_placeholder";
    return found;
  }
  std::set<fs::path> files;
  std::set<fs::path> skipped;
  for (const std::string& pattern : patterns) {
    if (TooBroad(pattern, roots)) {
      found.problem = "too_broad";
      found.path = pattern;
      continue;
    }
    Walker walker{.fold = roots.windows, .parts = Components(pattern)};
    walker.Step("/", 0);
    if (found.path.empty() || (!walker.files.empty() && files.empty())) {
      found.path = pattern;
      while (found.path.size() > 1 && found.path.ends_with('/')) found.path.pop_back();
    }
    files.merge(walker.files);
    skipped.merge(walker.skipped);
  }
  if (!files.empty()) found.problem.clear();
  for (const fs::path& file : files) {
    const auto logical = Logical(file, roots);
    if (!logical) continue;
    std::error_code ec;
    const auto bytes = fs::file_size(file, ec);
    const auto written = fs::last_write_time(file, ec);
    found.files.push_back({.root = logical->first,
                           .path = logical->second,
                           .absolute = file,
                           .bytes = ec ? 0 : bytes,
                           .mtime_ns = ec ? 0 : ToUnixNs(written)});
  }
  for (const fs::path& link : skipped) {
    if (const auto logical = Logical(link, roots))
      found.skipped.push_back(logical->first + ":" + logical->second);
  }
  return found;
}

std::vector<std::string> FileLocations(const json& entry, const Roots& roots) {
  std::vector<std::string> locations;
  const json files = entry.value("files", json::object());
  if (!files.is_object()) return locations;
  const std::string os = roots.windows ? "windows" : "linux";
  for (const auto& [pattern, info] : files.items()) {
    const json tags = info.is_object() ? info.value("tags", json::array()) : json::array();
    // Settings files aren't saves; an untagged location is taken as one.
    if (!tags.empty() && !std::ranges::contains(tags, json("save"))) continue;
    const json when = info.is_object() ? info.value("when", json::array()) : json::array();
    const bool applies =
        when.empty() || std::ranges::any_of(when, [&](const json& condition) {
          if (!condition.is_object()) return true;
          const std::string want_os = condition.value("os", "");
          const std::string want_store = condition.value("store", "");
          return (want_os.empty() || want_os == os) &&
                 (want_store.empty() || roots.store.empty() || want_store == roots.store);
        });
    if (applies) locations.push_back(pattern);
  }
  return locations;
}

}  // namespace mira::saves
