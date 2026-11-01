#include "library/Relocate.h"

#include <algorithm>
#include <format>
#include <system_error>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "emulators/Emulators.h"
#include "launchers/Launchers.h"
#include "library/FolderTags.h"
#include "library/GamePatch.h"
#include "library/PrefixNaming.h"
#include "library/Stores.h"

namespace mira::library {
namespace fs = std::filesystem;

namespace {

// Renames `from` to `to`, else copies it across and deletes it when `allow_copy`.
Result<void> MoveTree(const fs::path& from, const fs::path& to, bool allow_copy);

// Links inside `to` that pointed into `from` by an absolute path (a prefix's users/<name> as Lutris
// makes it) would dangle once it moved, so each is pointed at the same place in `to`.
void RepointLinks(const fs::path& from, const fs::path& to) {
  const fs::path old_root = from.lexically_normal();
  std::error_code ec;
  for (fs::recursive_directory_iterator it(to, fs::directory_options::skip_permission_denied, ec),
       end;
       !ec && it != end; it.increment(ec)) {
    std::error_code link_ec;
    if (!it->is_symlink(link_ec)) continue;
    const fs::path target = fs::read_symlink(it->path(), link_ec);
    if (link_ec || !target.is_absolute()) continue;
    const fs::path inside = target.lexically_normal().lexically_relative(old_root);
    if (inside.empty() || *inside.begin() == "..") continue;
    fs::remove(it->path(), link_ec);
    fs::create_symlink(inside == "." ? to : to / inside, it->path(), link_ec);
    if (link_ec)
      log::Warn("couldn't point {} at its new place: {}", it->path().string(), link_ec.message());
  }
}

}  // namespace

Result<void> MovePath(const fs::path& from, const fs::path& to, bool allow_copy) {
  Result<void> moved = MoveTree(from, to, allow_copy);
  std::error_code ec;
  if (moved && fs::is_directory(to, ec)) RepointLinks(from, to);
  return moved;
}

namespace {

Result<void> MoveTree(const fs::path& from, const fs::path& to, bool allow_copy) {
  std::error_code ec;
  if (!fs::exists(from, ec)) {
    if (ec)
      return Err("io_error", std::format("couldn't check {}: {}", from.string(), ec.message()));
    return Err("source_missing", std::format("{} doesn't exist", from.string()));
  }

  fs::create_directories(to.parent_path(), ec);
  fs::rename(from, to, ec);
  if (!ec) return {};
  if (ec != std::errc::cross_device_link) return Err("move_failed", ec.message());
  if (!allow_copy)
    return Err("cross_device", "target is on another filesystem and relocate.allow_copy is off");
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

// `inner`'s place under `outer`, carried over to `moved_outer`.
std::string Rebase(const fs::path& inner, const fs::path& outer, const fs::path& moved_outer) {
  const fs::path relative = inner.lexically_relative(outer);
  return (relative.empty() || relative == "." ? moved_outer : moved_outer / relative).string();
}

// Why a store-managed game's files stay, naming who keeps track of them.
std::string KeptBy(const model::Game& game) {
  if (game.source == "steam") return "Steam keeps track of them";
  if (const launchers::Launcher* launcher = launchers::ForGame(game))
    return launcher->name + " keeps track of them";
  if (const emulators::Emulator* emulator = emulators::ForGame(game))
    return emulator->name + " finds them there";
  if (const Store* store = FindStore(game.source))
    return std::string(store->name) + " keeps track of them";
  return "Their store keeps track of them";
}

RelocatePart Part(std::string action, std::string from, std::string reason = {}) {
  RelocatePart part;
  part.action = std::move(action);
  part.from = std::move(from);
  part.reason = std::move(reason);
  return part;
}

RelocatePart Move(const fs::path& from, const fs::path& to) {
  RelocatePart part = Part("move", from.string());
  part.to = to.string();
  part.copies = !OnSameDisk(from, to);
  return part;
}

}  // namespace

std::string SharedFolder(const config::Config& config, const model::Game& game,
                         std::span<const model::Game> library) {
  const std::vector<fs::path> install = {game.install_path};
  for (const fs::path& root : config.GetPathArray("library_roots")) {
    if (paths::IsWithin(root, install, /*allow_equal=*/true)) return "a library folder";
  }
  for (const model::Game& other : library) {
    if (other.id == game.id) continue;
    for (const std::string& path : {other.install_path, other.data_dir}) {
      if (!path.empty() && paths::IsWithin(path, install, /*allow_equal=*/true))
        return "\"" + other.name + "\"";
    }
  }
  return {};
}

bool RunsExternalProgram(const model::Game& game) {
  if (game.exe_path.empty()) return false;
  if (fs::path(StoredExePath(game.install_path, game.exe_path)).is_absolute())
    return true;  // outside its folder
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

Result<RelocatePlan> PlanRelocate(const config::Config& config, const model::Game& original,
                                  const RelocateRequest& request,
                                  std::span<const model::Game> library) {
  model::Game game = original;
  game.exe_path = StoredExePath(game.install_path, game.exe_path);
  // Store installs stay where their tool expects them unless a path is given.
  // Lutris only launches what it found, so its games are Mira's to move.
  // A store launcher's prefix holds the launcher and every game installed in it.
  const bool shared_prefix = launchers::ForGame(game) != nullptr;
  const bool store_managed = game.source == "steam" || game.source == "epic" ||
                             game.source == "gog" || game.source == "itch" ||
                             game.source == "amazon" || shared_prefix ||
                             emulators::ForGame(game) != nullptr;
  // Steam makes its own prefix (compatdata) again where it expects it, so a moved one is just left
  // behind.
  const bool steam_prefix = game.source == "steam" || game.runner_ref.starts_with("steam:");
  if (steam_prefix && request.data_dir) {
    return Err("store_prefix",
               std::format("{}'s prefix belongs to Steam, which keeps it in its compatdata folder",
                           game.name));
  }
  if (shared_prefix && request.data_dir) {
    return Err("shared_prefix",
               std::format("{}'s prefix is shared with its launcher and the launcher's other games",
                           game.name),
               "Move the launcher's prefix folder yourself, then point each of its games at the "
               "new folder.");
  }

  RelocatePlan plan;
  const bool both = !game.install_path.empty() && !game.data_dir.empty();
  plan.install_in_prefix =
      both && !request.install_path &&
      paths::IsWithin(game.install_path, {game.data_dir}, /*allow_equal=*/true);
  plan.prefix_in_install = both && !plan.install_in_prefix && !request.data_dir &&
                           paths::IsWithin(game.data_dir, {game.install_path});

  // The prefix's target is checked before anything moves, so a bad one never needs an undo.
  const bool move_prefix = !game.data_dir.empty() && !plan.prefix_in_install && !shared_prefix &&
                           !steam_prefix && (request.data_dir || !request.only_given);
  if (move_prefix && request.data_dir && !paths::IsWithin(*request.data_dir, PrefixRoots(config))) {
    return Err("path_outside_root",
               std::format("\"{}\" is not inside a prefix folder", request.data_dir->string()),
               "Pick a folder inside the prefix folder, or change that setting.",
               Fix::Setting("prefix_root"));
  }
  const bool allow_copy = config.GetBool("relocate.allow_copy");

  if (game.install_path.empty()) {
    plan.files = Part("none", {});
  } else if (plan.install_in_prefix) {
    plan.files = Part("follow", game.install_path, "Inside its prefix, so they move with it");
  } else if (!request.install_path && request.only_given) {
    plan.files = Part("in_place", game.install_path);
  } else if (!request.install_path && store_managed) {
    plan.files = Part("stay", game.install_path, KeptBy(game));
  } else if (!request.install_path && RunsExternalProgram(game)) {
    // A program the game only runs (an emulator in ~/Applications) stays where it is.
    plan.files = Part("stay", game.install_path, "The program it runs stays where it is");
  } else {
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
    if (target.empty()) {
      return Err("no_library_roots", "there's no library folder to move it into",
                 "Add a library folder.", Fix::Setting("library_roots"));
    }
    if (!paths::IsWithin(target, library_roots)) {
      return Err("path_outside_root",
                 std::format("\"{}\" is not inside a configured library root", target.string()),
                 "Pick a folder inside one of the library folders, or add it as one.",
                 Fix::Setting("library_roots"));
    }
    if (fs::path(game.install_path) == target) {
      plan.files = Part("in_place", game.install_path, "Already in Mira's folders");
    } else {
      // An AppImage carries everything it needs, so it moves on its own, whatever else its folder
      // holds.
      plan.single_file = RunsFromAppImage(game);
      const std::string shared =
          plan.single_file ? std::string() : SharedFolder(config, game, library);
      if (!shared.empty()) {
        return Err("shared_folder",
                   std::format("\"{}\" also holds {}, so it can't move with this game",
                               game.install_path, shared),
                   "Put the game in a folder of its own, then point the game at it.");
      }
      plan.files = plan.single_file
                       ? Move(fs::path(game.install_path) / game.exe_path, target / game.exe_path)
                       : Move(game.install_path, target);
      if (plan.files.copies && !allow_copy) {
        return Err(
            "cross_device",
            std::format("\"{}\" is on another drive and copying is turned off", target.string()),
            "Turn on copying across drives, or pick a folder on the same drive.",
            Fix::Setting("relocate.allow_copy"));
      }
      plan.install_target = target.string();
      game.install_path = target.string();  // the prefix's place follows the install's new disk
    }
  }

  if (game.data_dir.empty()) {
    plan.prefix = Part("none", {});
  } else if (plan.prefix_in_install) {
    plan.prefix = Part("follow", game.data_dir, "Inside the game's folder, so it moves with it");
  } else if (steam_prefix) {
    plan.prefix = Part("stay", game.data_dir, "Steam keeps it in its compatdata folder");
  } else if (shared_prefix) {
    plan.prefix = Part("stay", game.data_dir, "Shared with its launcher and its other games");
  } else if (!move_prefix) {
    plan.prefix = Part("in_place", game.data_dir);
  } else {
    const fs::path target =
        request.data_dir
            ? *request.data_dir
            : PrefixInGameFolder(config, game)
                  .value_or(NamedDir(config, game, PrefixRoot(config, game), game.data_dir));
    if (fs::path(game.data_dir) == target) {
      plan.prefix = Part("in_place", game.data_dir, "Already in Mira's folders");
    } else {
      plan.prefix = Move(game.data_dir, target);
      if (plan.prefix.copies && !allow_copy) {
        return Err(
            "cross_device",
            std::format("\"{}\" is on another drive and copying is turned off", target.string()),
            "Turn on copying across drives, or pick a folder on the same drive.",
            Fix::Setting("relocate.allow_copy"));
      }
    }
  }
  return plan;
}

Result<model::Game> Relocate(const config::Config& config, model::Game game,
                             const RelocateRequest& request, std::span<const model::Game> library) {
  const auto plan = PlanRelocate(config, game, request, library);
  if (!plan) return std::unexpected(plan.error());
  const std::string original_install = game.install_path;
  const std::string original_data = game.data_dir;
  game.exe_path = StoredExePath(game.install_path, game.exe_path);
  const bool allow_copy = config.GetBool("relocate.allow_copy");

  if (plan->files.action == "move") {
    if (auto moved = MovePath(plan->files.from, plan->files.to, allow_copy); !moved)
      return std::unexpected(moved.error());
    game.install_path = plan->install_target;
    if (plan->prefix_in_install && !plan->single_file)
      game.data_dir = Rebase(original_data, original_install, plan->install_target);
  }

  if (plan->prefix.action == "move") {
    if (auto moved = MovePath(plan->prefix.from, plan->prefix.to, allow_copy); !moved) {
      if (game.install_path == original_install) return std::unexpected(moved.error());
      // Put the install back, so the stored paths stay true.
      const Result<void> undone = MovePath(plan->files.to, plan->files.from, true);
      if (undone) return std::unexpected(moved.error());
      // It stays moved, so the game is returned with the install where it now is.
      log::Warn(
          "couldn't move {}'s prefix ({}) or put its install back ({}); the install stays at {}",
          game.id, moved.error().message, undone.error().message, game.install_path);
    } else {
      game.data_dir = plan->prefix.to;
      if (plan->install_in_prefix)
        game.install_path = Rebase(original_install, original_data, plan->prefix.to);
    }
  }

  // Moved away from where Lutris runs it, the game is Mira's own now. The
  // slug stays so a later Lutris import knows it and leaves it out.
  if (game.source == "lutris" &&
      (game.install_path != original_install || game.data_dir != original_data)) {
    game.source = "manual";
    game.source_ref = std::string(kClaimedLutrisPrefix) + game.source_ref;
  }
  return game;
}

}  // namespace mira::library
