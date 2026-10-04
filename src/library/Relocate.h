#pragma once

#include <filesystem>
#include <optional>

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
Result<model::Game> Relocate(const config::Config& config, model::Game game, const RelocateRequest& request = {});

}  // namespace mira::library
