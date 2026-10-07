#include "library/FolderTags.h"

#include <algorithm>
#include <format>
#include <system_error>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "library/Relocate.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

// Lexically normal, without a trailing separator, so "~/Games/" and "~/Games" compare equal.
fs::path Normal(const fs::path& path) {
  fs::path normal = path.lexically_normal();
  if (!normal.has_filename() && normal.has_parent_path() && normal != normal.root_path())
    normal = normal.parent_path();
  return normal;
}

bool IsUnder(const fs::path& path, const fs::path& base) {
  const auto [base_end, path_at] = std::ranges::mismatch(base, path);
  return base_end == base.end();
}

bool SameName(std::string_view a, std::string_view b) {
  return strings::ToLower(a) == strings::ToLower(b);
}

bool IsApplications(const fs::path& root) {
  return SameName(root.filename().string(), "applications");
}

// Folders Mira keeps for something else, which pruning must never remove.
bool IsConfiguredFolder(const config::Config& config, const fs::path& dir) {
  std::vector<fs::path> folders = {
      config.GetPath("prefix_root"), config.GetPath("epic.install_root"),
      config.GetPath("gog.install_root"), config.GetPath("itch.install_root"),
      config.GetPath("amazon.install_root")};
  for (const char* key : {"runner_search_paths", "wine_search_paths"}) {
    for (fs::path& path : config.GetPathArray(key)) folders.push_back(std::move(path));
  }
  return std::ranges::any_of(folders, [&](const fs::path& folder) {
    return !folder.empty() && IsUnder(Normal(folder), Normal(dir));
  });
}

// File managers drop these into any folder they show, so they don't keep one alive.
bool IsLeftover(const fs::path& path) {
  static constexpr std::string_view kLeftovers[] = {".directory", ".DS_Store", "Thumbs.db",
                                                    "desktop.ini"};
  std::error_code ec;
  return fs::is_regular_file(fs::symlink_status(path, ec)) &&
         std::ranges::contains(kLeftovers, path.filename().string());
}

std::optional<std::vector<std::string>> SortingOf(const SortRules& rules, const fs::path& root) {
  if (!std::ranges::contains(rules.sorted_roots, Normal(root))) return std::nullopt;
  return rules.folders;
}

// The shape sorting makes: root/<game>, root/.hidden/<game>, and a folder tag's folder in either.
// A game deeper than that was put there by hand and stays.
bool InSortedShape(const fs::path& root, const fs::path& parent) {
  const fs::path hidden = root / kHiddenFolder;
  return parent == root || parent == hidden || parent.parent_path() == root ||
         parent.parent_path() == hidden;
}

// Mira's own games: store installs stay where their tool expects them, Lutris only launches what
// it found, and a program a game only runs (an emulator) isn't the game's.
bool IsMiraOwned(const model::Game& game) {
  return (game.source == "scan" || game.source == "manual") && !game.install_path.empty() &&
         !RunsExternalProgram(game);
}

// Installed inside its prefix (an installer run in it put the game in drive_c).
bool InstalledInPrefix(const SortRules& rules, const model::Game& game) {
  const fs::path install = Normal(game.install_path);
  return (!rules.prefix_root.empty() && IsUnder(install, rules.prefix_root)) ||
         (!game.data_dir.empty() && IsUnder(install, Normal(game.data_dir)));
}

// A link's name: the game's name, made a valid file name.
std::string LinkName(const model::Game& game) {
  std::string name = game.name;
  std::ranges::replace(name, '/', '-');
  while (name.starts_with('.')) name.erase(0, 1);
  return name.empty() ? game.id : name;
}

bool IsLinkTo(const fs::path& link, const fs::path& target) {
  std::error_code ec;
  if (!fs::is_symlink(fs::symlink_status(link, ec))) return false;
  const fs::path points_at = fs::read_symlink(link, ec);
  return !ec && Normal(points_at) == Normal(target);
}

Result<model::Game> PlaceLink(const config::Config& config, model::Game game,
                              std::span<const model::Game> library) {
  const SortRules rules(config);
  const auto target = PlacedLinkPath(rules, game);
  const fs::path current = game.library_link;
  std::error_code ec;
  if (!target) {
    RemoveLink(game);
    game.library_link.clear();
    if (!current.empty()) PruneEmptyContainers(config, current.parent_path(), library);
    return game;
  }
  if (IsLinkTo(*target, game.install_path)) {
    game.library_link = target->string();
    return game;
  }
  // The game's own link still pointing where its folder used to be: replaced, not kept. Only ever
  // a link is removed, never a folder.
  if (!current.empty() && fs::is_symlink(fs::symlink_status(current, ec)) &&
      !IsLinkTo(current, game.install_path)) {
    fs::remove(current, ec);
  }
  if (fs::exists(fs::symlink_status(*target, ec))) {
    return Err(
        "target_exists",
        std::format("\"{}\" already exists, so {} has no link there", target->string(), game.name),
        "Rename or move that folder, or change the game's tags.");
  }
  fs::create_directories(target->parent_path(), ec);
  if (!current.empty() && IsLinkTo(current, game.install_path)) {
    fs::rename(current, *target, ec);
  } else {
    fs::create_directory_symlink(game.install_path, *target, ec);
  }
  if (ec) {
    PruneEmptyContainers(config, target->parent_path(), library);
    return Err("link_failed", std::format("couldn't link {} at {}: {}", game.name, target->string(),
                                          ec.message()));
  }
  game.library_link = target->string();
  return game;
}

}  // namespace

fs::path NormalRoot(const std::string& root) { return Normal(paths::Expand(root)); }

SortRules::SortRules(const config::Config& config)
    : folders(config.GetStringArray("tags.folders")),
      prefix_root(Normal(config.GetPath("prefix_root"))) {
  for (const std::string& root : config.GetStringArray("library_roots")) {
    roots.push_back(NormalRoot(root));
    roots_as_written.push_back(root);
  }
  for (const std::string& root : config.GetStringArray("tags.sorted_roots")) {
    sorted_roots.push_back(NormalRoot(root));
  }
}

bool IsLooseAppImage(const SortRules& rules, const model::Game& game) {
  if (!fs::path(game.exe_path).parent_path().empty() ||
      !strings::ToLower(game.exe_path).ends_with(".appimage")) {
    return false;
  }
  const fs::path root = RootOf(rules, game.install_path);
  return !root.empty() && ContainerOf(rules, root, game.install_path).has_value();
}

bool IsAtOrUnder(const fs::path& path, const fs::path& base) { return IsUnder(Normal(path), Normal(base)); }

fs::path RootOf(const SortRules& rules, const fs::path& path) {
  const fs::path normal = Normal(path);
  fs::path found;
  for (const fs::path& root : rules.roots) {
    if (!root.empty() && IsUnder(normal, root) &&
        std::distance(root.begin(), root.end()) > std::distance(found.begin(), found.end())) {
      found = root;
    }
  }
  return found;
}

bool SortsByTags(const SortRules& rules, const fs::path& root) {
  return SortingOf(rules, root).has_value();
}

std::vector<std::string> FolderTagsOf(const SortRules& rules, const fs::path& root) {
  return SortingOf(rules, root).value_or(std::vector<std::string>{});
}

std::string FolderTagFor(const SortRules& rules, const fs::path& root,
                         std::span<const std::string> tags, std::string_view pick) {
  const std::vector<std::string> folder_tags = FolderTagsOf(rules, root);
  const auto has = [&](std::string_view tag) {
    return std::ranges::any_of(tags, [&](const std::string& t) { return SameName(t, tag); });
  };
  if (!pick.empty() && has(pick)) {
    const auto match = std::ranges::find_if(
        folder_tags, [&](const std::string& folder) { return SameName(folder, pick); });
    if (match != folder_tags.end()) return *match;
  }
  const auto first = std::ranges::find_if(folder_tags, has);
  return first != folder_tags.end() ? *first : std::string();
}

void DropStalePick(model::Game& game) {
  if (!std::ranges::any_of(game.tags,
                           [&](const std::string& tag) { return SameName(tag, game.folder_tag); }))
    game.folder_tag.clear();
}

std::optional<Container> ContainerOf(const SortRules& rules, const fs::path& root,
                                     const fs::path& dir) {
  const fs::path base = Normal(root);
  const fs::path normal = Normal(dir);
  if (normal == base) return Container{};
  if (!SortsByTags(rules, base)) return std::nullopt;
  const fs::path hidden = base / kHiddenFolder;
  if (normal == hidden) return Container{.folder_tag = {}, .hidden = true};
  const fs::path parent = normal.parent_path();
  if (parent != base && parent != hidden) return std::nullopt;
  const std::string tag = FolderTagFor(rules, base, std::vector{normal.filename().string()});
  if (tag.empty()) return std::nullopt;
  return Container{.folder_tag = tag, .hidden = parent == hidden};
}

fs::path PlacedParent(const SortRules& rules, const fs::path& root, const model::Game& game) {
  fs::path parent = Normal(root);
  if (!SortsByTags(rules, parent)) return parent;
  if (std::ranges::contains(game.tags, std::string("hidden"))) parent /= kHiddenFolder;
  if (const std::string tag = FolderTagFor(rules, root, game.tags, game.folder_tag); !tag.empty())
    parent /= tag;
  return parent;
}

bool SortsByLink(const SortRules& rules, const model::Game& game) {
  return IsMiraOwned(game) && RootOf(rules, game.install_path).empty() &&
         InstalledInPrefix(rules, game);
}

fs::path SortRootOf(const SortRules& rules, const model::Game& game) {
  if (!IsMiraOwned(game)) return {};
  if (!SortsByLink(rules, game)) {
    const fs::path root = RootOf(rules, game.install_path);
    if (root.empty()) return {};
    const fs::path install = Normal(game.install_path);
    if (!IsLooseAppImage(rules, game) &&
        (install == root || !InSortedShape(root, install.parent_path()))) {
      return {};
    }
    return root;
  }
  const bool app = std::ranges::contains(game.tags, std::string("app"));
  const auto apps = std::ranges::find_if(rules.roots, IsApplications);
  const auto others =
      std::ranges::find_if(rules.roots, [](const fs::path& r) { return !IsApplications(r); });
  if (app && apps != rules.roots.end()) return *apps;
  if (others != rules.roots.end()) return *others;
  return rules.roots.empty() ? fs::path() : rules.roots.front();
}

std::optional<fs::path> PlacedInstallPath(const SortRules& rules, const model::Game& game) {
  if (SortsByLink(rules, game)) return std::nullopt;
  const fs::path root = SortRootOf(rules, game);
  if (root.empty() || !SortsByTags(rules, root)) return std::nullopt;
  const fs::path parent = PlacedParent(rules, root, game);
  if (IsLooseAppImage(rules, game)) return parent;
  return parent / Normal(game.install_path).filename();
}

std::optional<fs::path> PlacedLinkPath(const SortRules& rules, const model::Game& game) {
  if (!SortsByLink(rules, game)) return std::nullopt;
  const fs::path root = SortRootOf(rules, game);
  if (root.empty() || !SortsByTags(rules, root)) return std::nullopt;
  return PlacedParent(rules, root, game) / LinkName(game);
}

bool NeedsPlacing(const SortRules& rules, const model::Game& game) {
  if (SortsByLink(rules, game) || !game.library_link.empty()) {
    const auto target = PlacedLinkPath(rules, game);
    if (!target) return !game.library_link.empty();
    return Normal(game.library_link) != *target || !IsLinkTo(*target, game.install_path);
  }
  const auto target = PlacedInstallPath(rules, game);
  return target && *target != Normal(game.install_path);
}

fs::path SortingFolderOf(const SortRules& rules, const model::Game& game) {
  if (!game.library_link.empty()) return Normal(game.library_link).parent_path();
  const fs::path install = Normal(game.install_path);
  return IsLooseAppImage(rules, game) ? install : install.parent_path();
}

void RemoveLink(const model::Game& game) {
  if (game.library_link.empty()) return;
  std::error_code ec;
  if (fs::is_symlink(fs::symlink_status(game.library_link, ec))) fs::remove(game.library_link, ec);
}

Result<model::Game> Place(const config::Config& config, model::Game game,
                          std::span<const model::Game> library) {
  const SortRules rules(config);
  if (SortsByLink(rules, game) || !game.library_link.empty())
    return PlaceLink(config, game, library);
  const auto target = PlacedInstallPath(rules, game);
  if (!target || *target == Normal(game.install_path)) return game;

  const bool loose = IsLooseAppImage(rules, game);
  if (!loose) {
    if (const std::string shared = SharedFolder(config, game, library); !shared.empty()) {
      return Err("shared_folder",
                 std::format("\"{}\" also holds {}, so it can't move with this game",
                             game.install_path, shared),
                 "Put the game in a folder of its own, then point the game at it.");
    }
  }
  const fs::path from =
      loose ? fs::path(game.install_path) / game.exe_path : fs::path(game.install_path);
  const fs::path to = loose ? *target / game.exe_path : *target;

  std::error_code ec;
  if (fs::exists(fs::symlink_status(to, ec))) {
    return Err(
        "target_exists",
        std::format("\"{}\" already exists, so {} stays where it is", to.string(), game.name),
        "Rename or move that folder, or change the game's tags.");
  }
  fs::create_directories(to.parent_path(), ec);
  fs::rename(from, to, ec);
  if (ec) {
    PruneEmptyContainers(config, to.parent_path(), library);  // the folders made for it above
    if (ec == std::errc::cross_device_link) {
      return Err("cross_device",
                 std::format("\"{}\" is on another drive than \"{}\", so {} stays where it is",
                             to.parent_path().string(), from.string(), game.name),
                 "Folders are only sorted within one drive. Move the game yourself, or unmount "
                 "what's mounted there.");
    }
    return Err("move_failed",
               std::format("couldn't move {} to {}: {}", from.string(), to.string(), ec.message()));
  }

  // A prefix inside the game's folder moved with it.
  if (!loose && !game.data_dir.empty() &&
      IsUnder(Normal(game.data_dir), Normal(game.install_path))) {
    game.data_dir = (to / Normal(game.data_dir).lexically_relative(Normal(game.install_path)))
                        .lexically_normal()
                        .string();
  }
  game.install_path = target->string();
  return game;
}

void PruneEmptyContainers(const config::Config& config, const fs::path& dir,
                          std::span<const model::Game> library) {
  const fs::path root = RootOf(config, dir);
  if (root.empty()) return;
  const fs::path hidden = root / kHiddenFolder;
  for (fs::path current = Normal(dir); current != root; current = current.parent_path()) {
    const fs::path parent = current.parent_path();
    if (current != hidden && parent != root && parent != hidden) return;
    if (IsConfiguredFolder(config, current)) return;
    const bool used = std::ranges::any_of(library, [&](const model::Game& game) {
      return (!game.install_path.empty() && IsUnder(Normal(game.install_path), current)) ||
             (!game.library_link.empty() && IsUnder(Normal(game.library_link), current));
    });
    if (used) return;

    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(current, ec))) {
      if (fs::exists(fs::symlink_status(current, ec))) return;
      continue;  // already gone; its parent may still be empty
    }
    const std::vector<fs::path> entries = paths::ListDir(current);
    if (!std::ranges::all_of(entries, IsLeftover)) {
      log::Info("kept {}: it holds files that aren't a game", current.string());
      return;
    }
    for (const fs::path& entry : entries) fs::remove(entry, ec);
    if (!fs::remove(current, ec) || ec) {
      log::Warn("couldn't remove the empty folder {}: {}", current.string(), ec.message());
      return;
    }
    log::Info("removed the empty folder {}", current.string());
  }
}

}  // namespace mira::library
