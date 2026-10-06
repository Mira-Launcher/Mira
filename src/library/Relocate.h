#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"

namespace mira::library {

struct RelocateRequest {
  std::optional<std::filesystem::path> install_path;  // explicit target, else canonical
  std::optional<std::filesystem::path> data_dir;       // explicit target, else canonical
  bool only_given = false;  // leave a half without a target where it is, instead of canonical
};

// Moves game.install_path/data_dir to new locations -- explicit targets if
// given in `request`, else Mira's own canonical layout (a name-or-id leaf,
// per prefix_naming/NamedDir, under the first library_roots entry / under
// prefix_root). Either half no-ops if the game has no such path, or it's
// already at the target -- safe to run across a whole library without
// redundant moves. Every destination, explicit or canonical, must resolve
// inside a configured root (library_roots / prefix_root) the same way
// api::DeleteUnderRoot validates a deletion target, or the move is
// refused. A plain rename() where possible, falling back to copy-then-
// remove across filesystems (EXDEV). Never called automatically -- only
// from an explicit relocate request; changing prefix_root/prefix_naming
// itself moves nothing until this runs.
//
// A Lutris game moved out of Lutris's folders becomes a "manual" game whose
// source_ref is this plus its Lutris slug, so LutrisImporter skips that row.
inline constexpr std::string_view kClaimedLutrisPrefix = "lutris:";

// Why `game`'s install folder isn't the game's alone: it is or holds a
// library root, or holds another game in `library` (e.g. an AppImage sitting
// loose in a games folder). Empty when it's the game's alone.
std::string SharedFolder(const config::Config& config, const model::Game& game, std::span<const model::Game> library);

// The game is one AppImage file, which carries everything it needs: it moves
// and is deleted as that file alone, never with the folder it sits in.
bool RunsFromAppImage(const model::Game& game);

// The game runs a program that isn't its own files: an executable outside the
// game's folder, or an AppImage handed the game's file in its arguments (an
// emulator). Moving and deleting the game leave that program where it is.
bool RunsExternalProgram(const model::Game& game);

// When one folder is inside the other (a Lutris prefix that holds the
// game), the outer one moves and the inner follows, unless the inner has
// its own target. An install folder that also holds another game in
// `library`, or a library root, is refused rather than moved.
Result<model::Game> Relocate(const config::Config& config, model::Game game, const RelocateRequest& request = {},
                             std::span<const model::Game> library = {});

}  // namespace mira::library
