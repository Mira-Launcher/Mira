#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <json.hpp>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"

namespace mira::library {

// Games in a library folder listed in tags.sorted_roots are sorted by their tags (tags.folders):
// root/[.hidden/][<folder tag>/]<game>. The folders a game sits in are its
// tags made visible, so a move by hand changes the tags to match. A game
// installed inside its prefix gets a link there instead (Game::library_link).
inline constexpr std::string_view kHiddenFolder = ".hidden";

// A place games are sorted into under a library root.
struct Container {
  std::string folder_tag;  // as the setting spells it; empty at the root or in .hidden itself
  bool hidden = false;
};

// The settings sorting reads, taken once so a caller can also try folder tags not saved yet.
// Built from a Config wherever one is passed.
struct SortRules {
  SortRules(const config::Config& config);  // NOLINT: implicit, so callers pass their Config

  std::vector<std::filesystem::path> roots;   // normalised, in library_roots order
  std::vector<std::string> roots_as_written;  // the same, as library_roots spells them
  std::vector<std::string> folders;           // tags.folders, in order
  std::vector<std::filesystem::path> sorted_roots;  // tags.sorted_roots, normalised
  std::filesystem::path prefix_root;
  bool tag_by_root = true;
};

// Whether two tags are the same tag: folder names follow tags, so they match ignoring case.
bool SameTag(std::string_view a, std::string_view b);

// A library folder as SortRules keeps it ("~" expanded, normalised), from how a setting spells it.
std::filesystem::path NormalRoot(const std::string& root);

// Whether `path` is `base` or inside it, as written: no links resolved, and "~/Games/" and
// "~/Games" are the same folder.
bool IsAtOrUnder(const std::filesystem::path& path, const std::filesystem::path& base);

// The library root `path` is inside at any depth, or is; empty when none.
std::filesystem::path RootOf(const SortRules& rules, const std::filesystem::path& path);

// Whether `root` is in tags.sorted_roots. With no folder tags, a sorted root sorts only .hidden; a
// root not listed is never sorted.
bool SortsByTags(const SortRules& rules, const std::filesystem::path& root);

// The folder tags in `root`, in the order set: tags.folders when it's sorted, else none.
std::vector<std::string> FolderTagsOf(const SortRules& rules, const std::filesystem::path& root);

// The folder tag whose folder a game with `tags` goes in under `root`, as the setting spells it:
// `pick` (Game::folder_tag) while the game has it and it's a folder tag there, else the first of
// the root's folder tags, in tags.folders' order, that the game has. Empty when none.
std::string FolderTagFor(const SortRules& rules, const std::filesystem::path& root,
                         std::span<const std::string> tags, std::string_view pick = {});

// Clears `game`'s folder pick once it no longer has that tag, so adding the tag back later doesn't
// bring the pick back.
void DropStalePick(model::Game& game);

// What `dir` is under `root`: the root, .hidden, or a folder tag's folder in either. Folder names
// match tags ignoring case. Nullopt for any other folder.
std::optional<Container> ContainerOf(const SortRules& rules, const std::filesystem::path& root,
                                     const std::filesystem::path& dir);

// The game is one AppImage loose in a library root or a sorting folder in one: its install_path is
// that folder, so the file moves on its own.
bool IsLooseAppImage(const SortRules& rules, const model::Game& game);

// Where `game`'s tags put it under `root`.
std::filesystem::path PlacedParent(const SortRules& rules, const std::filesystem::path& root,
                                   const model::Game& game);

// The library root `game` is sorted in, whether or not that root sorts yet: the root its folder is
// in, or for a game installed inside its prefix (it gets a link instead), the Applications root
// for an `app` and the first other root for a game. Empty when the game is never sorted: a store
// install, a program it only runs, or a folder elsewhere.
std::filesystem::path SortRootOf(const SortRules& rules, const model::Game& game);

// Whether `game` is sorted by a link to its folder rather than by moving the folder.
bool SortsByLink(const SortRules& rules, const model::Game& game);

// Where `game`'s install_path belongs by its tags, or nullopt when its folder isn't moved (see
// SortRootOf, plus a root that doesn't sort, and a game linked instead). A loose AppImage's
// install_path is the folder it sits in, so that folder is what's placed.
std::optional<std::filesystem::path> PlacedInstallPath(const SortRules& rules,
                                                       const model::Game& game);

// Where a linked game's link belongs; nullopt when it should have none.
std::optional<std::filesystem::path> PlacedLinkPath(const SortRules& rules,
                                                    const model::Game& game);

// Whether Place would change anything for `game`.
bool NeedsPlacing(const SortRules& rules, const model::Game& game);

// The folder `game` sits in, as sorting sees it: its link's folder, a loose AppImage's
// install_path, else its parent.
std::filesystem::path SortingFolderOf(const SortRules& rules, const model::Game& game);

// Moves `game` (or its link) to where its tags put it by rename alone, never a copy: a target on
// another drive or one that already exists is refused and nothing moves. Returns the game with
// its new paths.
Result<model::Game> Place(const config::Config& config, model::Game game,
                          std::span<const model::Game> library);

// Removes `game`'s link, if it has one and it is still a link.
void RemoveLink(const model::Game& game);

// `game`'s tags changed to say it sits in `place` under `root`, having come from `old_root`: the
// reverse of Place, for a folder or link moved by hand. The game gets the place's folder tag, and
// it becomes the game's pick unless tags.folders' order already puts the game there; at the root
// level the root's folder tags and the pick are dropped; `hidden` follows .hidden; a new root
// swaps the scan.tag_by_root tag and `app`.
void TagsForPlace(const SortRules& rules, model::Game& game, const std::filesystem::path& old_root,
                  const std::filesystem::path& root, const Container& place);

// After a game left `dir`: removes it, then a .hidden above it, while no game in `library` is
// inside and nothing but file-manager leftovers (.directory, Thumbs.db, ...) remains. Only the
// folders sorting makes (root/X, root/.hidden, root/.hidden/X); never a root or a configured
// folder.
void PruneEmptyContainers(const config::Config& config, const std::filesystem::path& dir,
                          std::span<const model::Game> library);

}  // namespace mira::library
