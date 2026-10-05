#include "library/Relocate.h"

#include <algorithm>
#include <format>
#include <system_error>

#include "core/Paths.h"
#include "launchers/Launchers.h"
#include "library/PrefixNaming.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

Result<void> Move(const fs::path& from, const fs::path& to, bool allow_copy) {
  std::error_code ec;
  if (!fs::exists(from, ec)) return Err("source_missing", std::format("{} doesn't exist", from.string()));

  fs::create_directories(to.parent_path(), ec);
  fs::rename(from, to, ec);
  if (!ec) return {};
  if (ec != std::errc::cross_device_link) return Err("move_failed", ec.message());
  if (!allow_copy) return Err("cross_device", "target is on another filesystem and relocate.allow_copy is off");

  // rename() can't cross filesystems -- prefix_root and a game's own
  // install_path may be on different drives. Copy the whole tree, then
  // remove the source, same fallback any "move a directory" tool needs
  // once that's possible.
  fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
  if (ec) return Err("move_failed", ec.message());
  fs::remove_all(from, ec);
  if (ec) return Err("cleanup_failed", ec.message());
  return {};
}

// `inner`'s place under `outer`, carried over to `moved_outer`.
std::string Rebase(const fs::path& inner, const fs::path& outer, const fs::path& moved_outer) {
  const fs::path relative = inner.lexically_relative(outer);
  return (relative.empty() || relative == "." ? moved_outer : moved_outer / relative).string();
}

// Why `game`'s install folder can't move on its own: it holds a library
// root or another game's files (e.g. an exe sitting loose in a shared
// games folder). Empty when it's the game's alone.
std::string SharedFolder(const config::Config& config, const model::Game& game, std::span<const model::Game> library) {
  const std::vector<fs::path> install = {game.install_path};
  for (const fs::path& root : config.GetPathArray("library_roots")) {
    if (paths::IsWithin(root, install, /*allow_equal=*/true)) return "a library folder";
  }
  for (const model::Game& other : library) {
    if (other.id == game.id) continue;
    for (const std::string& path : {other.install_path, other.data_dir}) {
      if (!path.empty() && paths::IsWithin(path, install, /*allow_equal=*/true)) return "\"" + other.name + "\"";
    }
  }
  return {};
}

}  // namespace

Result<model::Game> Relocate(const config::Config& config, model::Game game, const RelocateRequest& request,
                             std::span<const model::Game> library) {
  // Store installs stay where their tool expects them unless a path is given.
  // Lutris only launches what it found, so its games are Mira's to move.
  const std::string original_install = game.install_path;
  const std::string original_data = game.data_dir;
  const bool store_managed = game.source == "steam" || game.source == "epic" || game.source == "gog" ||
                             game.source == "itch" || game.source == "amazon" || launchers::ForGame(game) != nullptr;
  const bool both = !game.install_path.empty() && !game.data_dir.empty();
  const bool install_in_prefix = both && !request.install_path &&
                                 paths::IsWithin(game.install_path, {game.data_dir}, /*allow_equal=*/true);
  const bool prefix_in_install =
      both && !install_in_prefix && !request.data_dir && paths::IsWithin(game.data_dir, {game.install_path});

  if (!game.install_path.empty() && !install_in_prefix &&
      (request.install_path || (!request.only_given && !store_managed))) {
    const std::vector<fs::path> library_roots = config.GetPathArray("library_roots");
    fs::path target;
    if (request.install_path) {
      target = *request.install_path;
    } else if (!config.GetString("relocate.install_root").empty()) {
      target = NamedDir(config, game, config.GetPath("relocate.install_root"), game.install_path);
    } else if (!library_roots.empty()) {
      target = NamedDir(config, game, library_roots.front(), game.install_path);
    }
    if (target.empty()) return Err("no_library_roots", "no library_roots configured to relocate into");
    if (!paths::IsWithin(target, library_roots)) {
      return Err("path_outside_root",
                 std::format("\"{}\" is not inside a configured library root", target.string()),
                 "Pick a folder inside one of the library folders, or add it as one.", Fix::Setting("library_roots"));
    }
    if (fs::path(game.install_path) != target) {
      if (const std::string shared = SharedFolder(config, game, library); !shared.empty()) {
        return Err("shared_folder",
                   std::format("\"{}\" also holds {}, so it can't move with this game", game.install_path, shared),
                   "Put the game in a folder of its own, then point the game at it.");
      }
      if (auto moved = Move(game.install_path, target, config.GetBool("relocate.allow_copy")); !moved) return std::unexpected(moved.error());
      game.install_path = target.string();
      if (prefix_in_install) game.data_dir = Rebase(original_data, original_install, target);
    }
  }

  if (!game.data_dir.empty() && !prefix_in_install && (request.data_dir || !request.only_given)) {
    const fs::path prefix_root = config.GetPath("prefix_root");
    const fs::path target = request.data_dir ? *request.data_dir
                                             : NamedDir(config, game, prefix_root, game.data_dir);
    // Put the install back on failure, so the stored paths stay true.
    const auto undo_install = [&] {
      if (game.install_path != original_install) (void)Move(game.install_path, original_install, true);
    };
    if (!paths::IsWithin(target, {prefix_root})) {
      undo_install();
      return Err("path_outside_root", std::format("\"{}\" is not inside prefix_root", target.string()),
                 "Pick a folder inside the prefix folder, or change that setting.", Fix::Setting("prefix_root"));
    }
    if (fs::path(game.data_dir) != target) {
      if (auto moved = Move(game.data_dir, target, config.GetBool("relocate.allow_copy")); !moved) {
        undo_install();
        return std::unexpected(moved.error());
      }
      game.data_dir = target.string();
      if (install_in_prefix) game.install_path = Rebase(original_install, original_data, target);
    }
  }

  // Moved away from where Lutris runs it, the game is Mira's own now. The
  // slug stays so a later Lutris import knows it and leaves it out.
  if (game.source == "lutris" && (game.install_path != original_install || game.data_dir != original_data)) {
    game.source = "manual";
    game.source_ref = std::string(kClaimedLutrisPrefix) + game.source_ref;
  }
  return game;
}

}  // namespace mira::library
