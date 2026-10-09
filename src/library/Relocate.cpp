#include "library/Relocate.h"

#include <algorithm>
#include <format>
#include <system_error>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "launchers/Launchers.h"
#include "library/FolderTags.h"
#include "library/GamePatch.h"
#include "library/PrefixNaming.h"

namespace mira::library {
namespace fs = std::filesystem;

Result<void> MovePath(const fs::path& from, const fs::path& to, bool allow_copy) {
  std::error_code ec;
  if (!fs::exists(from, ec)) return Err("source_missing", std::format("{} doesn't exist", from.string()));

  fs::create_directories(to.parent_path(), ec);
  fs::rename(from, to, ec);
  if (!ec) return {};
  if (ec != std::errc::cross_device_link) return Err("move_failed", ec.message());
  if (!allow_copy) return Err("cross_device", "target is on another filesystem and relocate.allow_copy is off");
  if (fs::exists(to, ec)) return Err("move_failed", std::format("{} already exists", to.string()));

  // rename() can't cross filesystems -- prefix_root and a game's own
  // install_path may be on different drives. Copy the whole tree, then
  // remove the source, same fallback any "move a directory" tool needs
  // once that's possible.
  fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
  if (ec) {
    const std::string message = ec.message();
    fs::remove_all(to, ec);  // a half copy would block a retry and leave two copies
    return Err("move_failed", message);
  }
  // The copy is whole, so the move succeeded; what's left of the source is only clutter.
  fs::remove_all(from, ec);
  if (ec)
    log::Warn("moved {} to {}, but couldn't remove all of the old copy: {}", from.string(),
              to.string(), ec.message());
  return {};
}

namespace {

// `inner`'s place under `outer`, carried over to `moved_outer`.
std::string Rebase(const fs::path& inner, const fs::path& outer, const fs::path& moved_outer) {
  const fs::path relative = inner.lexically_relative(outer);
  return (relative.empty() || relative == "." ? moved_outer : moved_outer / relative).string();
}

}  // namespace

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

bool RunsExternalProgram(const model::Game& game) {
  if (game.exe_path.empty()) return false;
  if (fs::path(StoredExePath(game.install_path, game.exe_path)).is_absolute()) return true;  // outside its folder
  if (!strings::ToLower(game.exe_path).ends_with(".appimage")) return false;
  std::error_code ec;
  return std::ranges::any_of(strings::SplitArgs(game.args), [&](const std::string& arg) {
    return fs::path(arg).is_absolute() && fs::is_regular_file(arg, ec);
  });
}

bool RunsFromAppImage(const model::Game& game) {
  std::error_code ec;
  return strings::ToLower(game.exe_path).ends_with(".appimage") && !RunsExternalProgram(game) &&
         fs::is_regular_file(fs::path(game.install_path) / game.exe_path, ec);
}

Result<model::Game> Relocate(const config::Config& config, model::Game game, const RelocateRequest& request,
                             std::span<const model::Game> library) {
  // Store installs stay where their tool expects them unless a path is given.
  // Lutris only launches what it found, so its games are Mira's to move.
  const std::string original_install = game.install_path;
  const std::string original_data = game.data_dir;
  game.exe_path = StoredExePath(game.install_path, game.exe_path);
  // A store launcher's prefix holds the launcher and every game installed in it.
  const bool shared_prefix = launchers::ForGame(game) != nullptr;
  const bool store_managed = game.source == "steam" || game.source == "epic" || game.source == "gog" ||
                             game.source == "itch" || game.source == "amazon" || shared_prefix;
  // Steam makes its own prefix (compatdata) again where it expects it, so a moved one is just left behind.
  const bool steam_prefix = game.source == "steam" || game.runner_ref.starts_with("steam:");
  if (steam_prefix && request.data_dir) {
    return Err("store_prefix", std::format("{}'s prefix belongs to Steam, which keeps it in its compatdata folder", game.name));
  }
  if (shared_prefix && request.data_dir) {
    return Err("shared_prefix", std::format("{}'s prefix is shared with its launcher and the launcher's other games", game.name),
               "Move the launcher's prefix folder yourself, then point each of its games at the new folder.");
  }
  const bool both = !game.install_path.empty() && !game.data_dir.empty();
  const bool install_in_prefix = both && !request.install_path &&
                                 paths::IsWithin(game.install_path, {game.data_dir}, /*allow_equal=*/true);
  const bool prefix_in_install =
      both && !install_in_prefix && !request.data_dir && paths::IsWithin(game.data_dir, {game.install_path});
  bool single_file = false;  // only the program moved, out of a shared folder

  // The prefix's target is checked before anything moves, so a bad one never needs an undo.
  const bool move_prefix = !game.data_dir.empty() && !prefix_in_install && !shared_prefix && !steam_prefix &&
                           (request.data_dir || !request.only_given);
  const fs::path prefix_root = config.GetPath("prefix_root");
  const fs::path prefix_target = !move_prefix ? fs::path()
                                 : request.data_dir
                                     ? *request.data_dir
                                     : NamedDir(config, game, prefix_root, game.data_dir);
  if (move_prefix && !paths::IsWithin(prefix_target, {prefix_root})) {
    return Err("path_outside_root",
               std::format("\"{}\" is not inside prefix_root", prefix_target.string()),
               "Pick a folder inside the prefix folder, or change that setting.",
               Fix::Setting("prefix_root"));
  }

  // A program the game only runs (an emulator in ~/Applications) stays where it is.
  if (!game.install_path.empty() && !install_in_prefix &&
      (request.install_path || (!request.only_given && !store_managed && !RunsExternalProgram(game)))) {
    const std::vector<fs::path> library_roots = config.GetPathArray("library_roots");
    fs::path target;
    if (request.install_path) {
      target = *request.install_path;
    } else if (!config.GetString("relocate.install_root").empty()) {
      target = NamedDir(config, game,
                        PlacedParent(config, config.GetPath("relocate.install_root"), game),
                        game.install_path);
    } else if (!library_roots.empty()) {
      target = NamedDir(config, game, PlacedParent(config, library_roots.front(), game),
                        game.install_path);
    }
    if (target.empty()) return Err("no_library_roots", "no library_roots configured to relocate into");
    if (!paths::IsWithin(target, library_roots)) {
      return Err("path_outside_root",
                 std::format("\"{}\" is not inside a configured library root", target.string()),
                 "Pick a folder inside one of the library folders, or add it as one.", Fix::Setting("library_roots"));
    }
    if (fs::path(game.install_path) != target) {
      // An AppImage carries everything it needs, so it moves on its own, whatever else its folder holds.
      const fs::path program = fs::path(game.install_path) / game.exe_path;
      single_file = RunsFromAppImage(game);
      const std::string shared = single_file ? std::string() : SharedFolder(config, game, library);
      if (!shared.empty()) {
        return Err("shared_folder",
                   std::format("\"{}\" also holds {}, so it can't move with this game", game.install_path, shared),
                   "Put the game in a folder of its own, then point the game at it.");
      }
      const fs::path from = single_file ? program : fs::path(game.install_path);
      const fs::path to = single_file ? target / game.exe_path : target;
      if (auto moved = MovePath(from, to, config.GetBool("relocate.allow_copy")); !moved) return std::unexpected(moved.error());
      game.install_path = target.string();
      if (prefix_in_install && !single_file) game.data_dir = Rebase(original_data, original_install, target);
    }
  }

  if (move_prefix && fs::path(game.data_dir) != prefix_target) {
    if (auto moved = MovePath(game.data_dir, prefix_target, config.GetBool("relocate.allow_copy"));
        !moved) {
      if (game.install_path == original_install) return std::unexpected(moved.error());
      // Put the install back, so the stored paths stay true.
      const Result<void> undone = single_file
                                      ? MovePath(fs::path(game.install_path) / game.exe_path,
                                             fs::path(original_install) / game.exe_path, true)
                                      : MovePath(game.install_path, original_install, true);
      if (undone) return std::unexpected(moved.error());
      // It stays moved, so the game is returned with the install where it now is.
      log::Warn(
          "couldn't move {}'s prefix ({}) or put its install back ({}); the install stays at {}",
          game.id, moved.error().message, undone.error().message, game.install_path);
    } else {
      game.data_dir = prefix_target.string();
      if (install_in_prefix)
        game.install_path = Rebase(original_install, original_data, prefix_target);
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
