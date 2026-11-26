#include "library/SourceRemoval.h"

#include <sys/stat.h>

#include <algorithm>
#include <filesystem>
#include <format>

#include "amazon/Nile.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "emulators/Emulators.h"
#include "epic/Legendary.h"
#include "itch/Butlerd.h"
#include "itch/Itch.h"
#include "launchers/Launchers.h"
#include "library/PrefixNaming.h"
#include "library/Relocate.h"
#include "library/Stores.h"
#include "metadata/MetadataFetcher.h"
#include "modmanagers/ModManagers.h"
#include "modmanagers/Purge.h"
#include "system/Packages.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

bool IsStore(std::string_view source) {
  return source == "epic" || source == "gog" || source == "itch" || source == "amazon";
}

// Only the app owns these games' files. An emulator's games are the user's own, kept when it goes.
bool FilesOwnedElsewhere(std::string_view source) {
  return source == "steam" || source == "lutris" || source == "heroic" || source == "humble" ||
         emulators::Find(source) != nullptr || modmanagers::Find(source) != nullptr;
}

bool Belongs(const model::Game& game, std::string_view source,
             const launchers::Launcher* launcher) {
  if (game.source == source) return true;
  return launcher != nullptr && game.id == launchers::GameId(*launcher);
}

std::vector<fs::path> DeleteRoots(const config::Config& config, const std::string& prefix) {
  std::vector<fs::path> roots = config.GetPathArray("library_roots");
  std::ranges::move(config::StoreInstallRoots(config), std::back_inserter(roots));
  // A launcher game installed inside its prefix.
  if (!prefix.empty()) roots.push_back(fs::path(prefix) / "drive_c");
  return roots;
}

// A game's own files outside Mira's folders that it may delete when asked: under home or a
// mounted drive, and neither one of Mira's folders nor holding one.
bool DeletableOutside(const config::Config& config, const fs::path& path) {
  std::error_code ec;
  const fs::path target = fs::weakly_canonical(path, ec);
  if (ec || !fs::exists(target, ec)) return false;
  if (!paths::IsWithin(target, {paths::Home(), "/mnt", "/media", "/run/media"})) return false;
  // Not a whole drive, nor one of home's own folders (Documents, .local).
  struct stat own{}, parent{};
  if (::stat(target.c_str(), &own) != 0 || ::stat(target.parent_path().c_str(), &parent) != 0 ||
      own.st_dev != parent.st_dev)
    return false;
  const fs::path home = fs::weakly_canonical(paths::Home(), ec);
  static constexpr std::string_view kHomeFolders[] = {"Desktop", "Documents", "Downloads",
                                                      "Music",   "Pictures",  "Videos",
                                                      "Public",  "Templates", "Games"};
  const fs::path in_home = target.lexically_relative(home);
  if (!in_home.empty() && *in_home.begin() != "..") {
    const std::string top = in_home.begin()->string();
    if (top.starts_with('.')) return false;
    if (std::distance(in_home.begin(), in_home.end()) == 1 &&
        std::ranges::contains(kHomeFolders, top))
      return false;
  }
  std::vector<fs::path> mira = DeleteRoots(config, {});
  std::ranges::move(PrefixRoots(config), std::back_inserter(mira));
  std::ranges::move(config::PathsOf(config, "emulators.roms_root"), std::back_inserter(mira));
  mira.push_back(config.File().parent_path());
  return std::ranges::none_of(mira, [&](const fs::path& folder) {
    return !folder.empty() && paths::IsWithin(folder, {target}, /*allow_equal=*/true);
  });
}

std::string WhatGetsDeleted(const model::Game& game, std::string_view source) {
  if (FilesOwnedElsewhere(source) || game.install_path.empty()) return {};
  if (source == "epic") return "legendary uninstall " + game.source_ref;
  if (source == "amazon") return "nile uninstall " + game.source_ref;
  if (source == "itch") return "butler uninstall (" + game.install_path + ")";
  if (source == "office") return "Office's installer removes " + game.name;
  return game.install_path;
}

Result<void> UninstallItch(const config::Config& config, const std::string& game_ref) {
  const Result<std::int64_t> profile_id = itch::CurrentProfileId(config);
  if (!profile_id) return std::unexpected(profile_id.error());
  const Result<json> caves = itch::Call(config, "Fetch.Caves", {{"profileId", *profile_id}});
  if (!caves) return std::unexpected(caves.error());
  for (const json& cave : caves->value("items", json::array())) {
    if (std::to_string(cave.value("game", json::object()).value("id", std::int64_t{0})) != game_ref)
      continue;
    if (auto done = itch::CallLong(config, "Uninstall.Perform",
                                   {{"caveId", cave.value("id", std::string())}});
        !done) {
      return std::unexpected(done.error());
    }
  }
  return {};
}

Result<void> UninstallGame(const config::Config& config, const model::Game& game,
                           std::string_view source, std::span<const model::Game> library) {
  if (FilesOwnedElsewhere(source) || game.install_path.empty()) return {};
  return DeleteGameFiles(config, game, library);
}

// Deletes everything in `dir` except the `keep` entries (by name).
Result<void> DeleteKeeping(const fs::path& dir, const std::vector<std::string>& keep,
                           const fs::path& prefix) {
  std::error_code ec;
  if (!fs::exists(dir, ec)) return {};
  for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec)) {
    if (std::ranges::contains(keep, entry.path().filename().string())) continue;
    if (auto deleted = DeleteInside(entry.path().string(), {prefix / "drive_c"}); !deleted)
      return deleted;
  }
  if (keep.empty() || fs::is_empty(dir, ec))
    return DeleteInside(dir.string(), {prefix / "drive_c"});
  return {};
}

// A launcher's program folder, relative to drive_c. EA keeps versioned
// folders one level up from its exe, Creative Cloud its helpers.
fs::path ProgramDir(const launchers::Launcher& launcher) {
  const fs::path dir = fs::path(launcher.exe).parent_path();
  if (launcher.id == "office") return "Program Files/Microsoft Office";
  if (launcher.id == "adobe") return dir.parent_path();
  return launcher.id == "ea" ? dir.parent_path() : dir;
}

// A launcher's own apps (Word and Excel under Microsoft 365) live in its program folder, which goes
// with the launcher: they have no files of their own to delete, and their folder is no "shared"
// one.
bool InLauncherFolder(const model::Game& game, const launchers::Launcher* launcher) {
  if (launcher == nullptr || game.install_path.empty() || game.data_dir.empty()) return false;
  return paths::IsWithin(fs::path(game.install_path),
                         {fs::path(game.data_dir) / "drive_c" / ProgramDir(*launcher)});
}

// Save folders some launchers keep inside their own program folder.
std::vector<std::string> KeptInProgramDir(std::string_view launcher) {
  if (launcher == "ubisoft") return {"savegames"};
  return {};
}

// The store tool DeleteGameFiles uninstalls a game through, by name; "" when Mira deletes its
// files itself.
std::string UninstallTool(const model::Game& game) {
  if (game.source == "office") return "Office";
  if (game.source_ref.empty()) return {};
  if (game.source == "epic") return "Legendary";
  if (game.source == "amazon") return "Nile";
  if (game.source == "itch") return "butler";
  return {};
}

// What DeleteGameFiles deletes itself for a game no store tool uninstalls, and the folders it may
// delete them from.
struct OwnFiles {
  std::vector<fs::path> paths;
  std::vector<fs::path> roots;
};

Result<OwnFiles> FilesToDelete(const config::Config& config, const model::Game& game,
                               std::span<const model::Game> library) {
  if (const emulators::Emulator* emulator = emulators::ForGame(game)) {
    OwnFiles files{.paths = emulators::GameFiles(config, game),
                   .roots = emulators::GameRoots(config, *emulator)};
    std::ranges::move(config::PathsOf(config, "emulators.roms_root"),
                      std::back_inserter(files.roots));
    return files;
  }
  // A program the game only runs (an emulator) isn't the game's to delete.
  if (RunsExternalProgram(game)) return OwnFiles{};
  OwnFiles files;
  files.roots = DeleteRoots(config, game.data_dir);
  // An AppImage is the whole game, so only that file goes, wherever it sits.
  if (RunsFromAppImage(game)) {
    files.paths = {fs::path(game.install_path) / game.exe_path};
    return files;
  }
  if (const std::string shared = SharedFolder(config, game, library); !shared.empty()) {
    return Err("shared_folder",
               std::format("\"{}\" also holds {}, so its files weren't deleted", game.install_path,
                           shared),
               "Delete the files by hand, or move the game to a folder of its own first.");
  }
  files.paths = {game.install_path};
  return files;
}

}  // namespace

Result<void> DeleteInside(const std::string& target, const std::vector<fs::path>& roots) {
  if (target.empty()) return Err("nothing_to_delete", "no path recorded");
  std::error_code ec;
  const fs::path resolved = fs::weakly_canonical(target, ec);
  if (ec) return Err("path_error", ec.message());
  if (!paths::IsWithin(resolved, roots))
    return Err("path_outside_root",
               std::format("\"{}\" isn't inside a Mira folder, so it was left alone", target));
  fs::remove_all(resolved, ec);
  if (ec) return Err("delete_failed", ec.message());
  return {};
}

Result<void> DeleteGameFiles(const config::Config& config, const model::Game& game,
                             std::span<const model::Game> library, bool outside) {
  if (game.source == "epic" && !game.source_ref.empty()) {
    if (auto done = epic::RunLegendary(config, {"uninstall", game.source_ref, "-y"}); !done) {
      return std::unexpected(done.error());
    }
    return {};
  }
  if (game.source == "amazon" && !game.source_ref.empty()) {
    if (auto done = amazon::RunNile(config, {"uninstall", game.source_ref}); !done)
      return std::unexpected(done.error());
    return {};
  }
  if (game.source == "itch" && !game.source_ref.empty())
    return UninstallItch(config, game.source_ref);
  const Result<OwnFiles> files = FilesToDelete(config, game, library);
  if (!files) return std::unexpected(files.error());
  // Only what the plan calls the game's own, or its store's (a GOG game imported from elsewhere):
  // another app's files (Heroic's, Lutris's) stay.
  if (outside) {
    const std::string kind = PlanGameRemoval(config, game, library).kind;
    outside = kind == "outside" || kind == "store";
  }
  for (const fs::path& path : files->paths) {
    std::vector<fs::path> roots = files->roots;
    if (outside && !paths::IsWithin(path, roots) && DeletableOutside(config, path))
      roots.push_back(fs::weakly_canonical(path).parent_path());
    if (auto deleted = DeleteInside(path.string(), roots); !deleted) return deleted;
  }
  return {};
}

GameRemoval PlanGameRemoval(const config::Config& config, const model::Game& game,
                            std::span<const model::Game> library) {
  GameRemoval plan;
  plan.id = game.id;
  plan.name = game.name;
  std::error_code ec;
  const auto same = [&ec](const std::string& a, const std::string& b) {
    return !a.empty() && !b.empty() && fs::weakly_canonical(a, ec) == fs::weakly_canonical(b, ec);
  };
  // Only a prefix Mira made; Steam deletes its own with the game.
  if (game.source != "steam" && fs::is_directory(game.data_dir, ec) &&
      paths::IsWithin(game.data_dir, PrefixRoots(config))) {
    plan.prefix = game.data_dir;
    for (const model::Game& other : library) {
      if (other.id != game.id && same(other.data_dir, game.data_dir)) {
        plan.prefix_shared_with = other.name;
        break;
      }
    }
  }
  if (game.source == "steam") {
    plan.kind = "steam";
    plan.tool = "Steam";
    plan.files = game.install_path;
    return plan;
  }
  plan.tool = UninstallTool(game);
  if (!plan.tool.empty() || (FindStore(game.source) != nullptr && !game.install_path.empty())) {
    plan.kind = "store";
    plan.files = game.install_path;
    return plan;
  }
  const launchers::Launcher* launcher = launchers::Find(game.source);
  plan.kind = "external";
  // A ROM in its emulator's folders is deleted with it, or the next scan brings it back.
  const bool rom = emulators::ForGame(game) != nullptr;
  if (game.source == "desktop-entry" || (FilesOwnedElsewhere(game.source) && !rom) ||
      launcher != nullptr || (game.source == "launcher" && game.install_path.empty())) {
    return plan;
  }
  // A game whose folder is its prefix (an old import's) goes with the prefix.
  if (!plan.prefix.empty() && same(game.install_path, game.data_dir)) {
    plan.kind = "folder";
    plan.files = game.install_path;
    plan.folder_is_prefix = true;
    return plan;
  }
  const Result<OwnFiles> files = FilesToDelete(config, game, library);
  if (!files || files->paths.empty()) return plan;
  const bool inside = std::ranges::all_of(
      files->paths, [&](const fs::path& path) { return paths::IsWithin(path, files->roots); });
  // Outside Mira's folders its files are the user's: deleted only when asked.
  if (!inside && !std::ranges::all_of(files->paths, [&](const fs::path& path) {
        return DeletableOutside(config, path);
      }))
    return plan;
  plan.kind = inside ? "folder" : "outside";
  plan.files = files->paths.front().string();
  const fs::path drive_c = fs::path(game.data_dir) / "drive_c";
  if (!plan.prefix.empty() && paths::IsWithin(plan.files, {drive_c})) {
    std::string inside = fs::weakly_canonical(plan.files, ec)
                             .lexically_relative(fs::weakly_canonical(drive_c, ec))
                             .string();
    std::ranges::replace(inside, '/', '\\');
    plan.files_in_prefix = "C:\\" + inside;
  }
  return plan;
}

Result<RemovalPlan> PlanRemoval(const config::Config& config, const store::GameStore& games,
                                std::string_view source) {
  const launchers::Launcher* launcher = launchers::Find(source);
  if (!launcher && !IsStore(source) && !FilesOwnedElsewhere(source)) {
    return Err("unknown_source", std::format("no source named \"{}\"", source));
  }
  RemovalPlan plan;
  plan.source = std::string(source);
  plan.signs_out = IsStore(source);
  if (const Store* store = FindStore(source)) {
    const auto add = [&](const char* name, const runner::ToolStatus& tool) {
      if (tool.source == "managed")
        plan.tools.push_back({.name = name, .path = fs::path(tool.path).parent_path().string()});
    };
    add(store->tool, store->status(config).tool);
    if (store->helper_status) add(store->helper, store->helper_status(config));
  }
  if (const emulators::Emulator* emulator = emulators::Find(source)) {
    plan.program_dir = emulators::DownloadDir(config, *emulator).string();
    if (!emulators::Downloads(*emulator)) {
      plan.packages = system::InstalledPackages(emulator->id);
      plan.uninstall = system::UninstallCommand(system::DetectDistro().family, plan.packages);
    }
    for (const fs::path& path : emulators::DataPaths(*emulator)) plan.data.push_back(path.string());
  }
  if (const modmanagers::ModManager* manager = modmanagers::Find(source)) {
    plan.program_dir = modmanagers::Dir(*manager).string();
    for (const model::Game& game : games.All()) {
      if (modmanagers::OnGame(game, *manager)) plan.purged.push_back({game.id, game.name, {}});
    }
  }
  for (const model::Game& game : games.All()) {
    if (!Belongs(game, source, launcher)) continue;
    if (!game.data_dir.empty() && !std::ranges::contains(plan.kept, game.data_dir))
      plan.kept.push_back(game.data_dir);
    if (launcher != nullptr && game.id == launchers::GameId(*launcher)) {
      plan.program_dir = (fs::path(game.data_dir) / "drive_c" / ProgramDir(*launcher)).string();
      for (const std::string& keep : KeptInProgramDir(source)) {
        plan.kept.push_back((fs::path(plan.program_dir) / keep).string());
      }
      continue;
    }
    plan.games.push_back({.id = game.id,
                          .name = game.name,
                          .deletes = InLauncherFolder(game, launcher)
                                         ? std::string()
                                         : WhatGetsDeleted(game, source)});
  }
  if (const emulators::Emulator* emulator = emulators::Find(source)) {
    const std::vector<model::Game> library = games.All();
    std::vector<const emulators::Emulator*> targets;
    bool first = true;
    for (const model::Game& game : library) {
      if (game.source != emulator->id) continue;
      if (PlanGameRemoval(config, game, library).kind == "folder") ++plan.roms;
      std::vector<const emulators::Emulator*> takes = emulators::Alternatives(config, game);
      if (!first)
        std::erase_if(targets, [&](const auto* to) { return !std::ranges::contains(takes, to); });
      else
        targets = std::move(takes);
      first = false;
    }
    for (const emulators::Emulator* to : targets) plan.move_to.push_back(to->id);
  }
  return plan;
}

Result<RemovalResult> RemoveSource(config::Config& config, store::GameStore& games,
                                   api::EventBus& events, std::string_view source, bool delete_data,
                                   const RomChoice& roms) {
  const Result<RemovalPlan> plan = PlanRemoval(config, games, source);
  if (!plan) return std::unexpected(plan.error());
  const launchers::Launcher* launcher = launchers::Find(source);
  const emulators::Emulator* move_to = nullptr;
  if (!roms.move_to.empty()) {
    if (!std::ranges::contains(plan->move_to, roms.move_to))
      return Err("cant_move", std::format("{}'s games can't move to {}", source, roms.move_to));
    move_to = emulators::Find(roms.move_to);
  }

  RemovalResult result;
  const auto forget = [&](const std::string& id) {
    games.Metadata().Remove(id);
    if (games.Remove(id)) {
      events.Publish("game.removed", {{"id", id}});
      ++result.removed;
    }
  };

  // One copy of the library, less each game as it goes, rather than a copy per game.
  std::vector<model::Game> library = games.All();
  for (const RemovalGame& planned : plan->games) {
    const auto folders_lock = games.LockFolders();
    const std::optional<model::Game> game = games.Find(planned.id);
    if (!game) continue;
    // Only what's in Mira's folders moves; a game kept where the user put it stays there.
    if (move_to != nullptr && PlanGameRemoval(config, *game, library).kind == "folder") {
      if (auto moved = emulators::MoveGame(config, games, *game, *move_to); moved) {
        events.Publish("game.updated", model::ToJson(*moved));
        std::erase_if(library, [&](const model::Game& known) { return known.id == game->id; });
        continue;
      } else {
        result.problems.push_back(std::format("{}: {}", game->name, moved.error().message));
      }
    }
    // A game that didn't move keeps its file.
    const bool rom = move_to == nullptr && roms.delete_files &&
                     emulators::ForGame(*game) != nullptr &&
                     PlanGameRemoval(config, *game, library).kind == "folder";
    if (InLauncherFolder(*game, launcher)) {
      // Its files go with the launcher's program folder, below.
    } else if (auto done = rom ? DeleteGameFiles(config, *game, library)
                               : UninstallGame(config, *game, source, library);
               !done) {
      // Files stay; the record goes anyway so the source can still be removed.
      result.problems.push_back(std::format("{}: {}", game->name, done.error().message));
    }
    forget(game->id);
    std::erase_if(library, [&](const model::Game& known) { return known.id == game->id; });
  }

  if (launcher != nullptr) {
    if (const std::optional<model::Game> entry = games.Find(launchers::GameId(*launcher))) {
      const fs::path program = fs::path(entry->data_dir) / "drive_c" / ProgramDir(*launcher);
      if (auto done = DeleteKeeping(program, KeptInProgramDir(source), entry->data_dir); !done) {
        result.problems.push_back(std::format("{}: {}", launcher->name, done.error().message));
      }
      forget(entry->id);
    }
  }

  // Undone on every game it was added to, its mods and settings there deleted, before its program
  // goes. A game it fails on loses it all the same, and the failure is reported.
  if (const modmanagers::ModManager* manager = modmanagers::Find(source)) {
    for (const model::Game& game : games.All()) {
      if (!modmanagers::OnGame(game, *manager)) continue;
      if (auto purged = modmanagers::PurgeFromGame(games, *manager, game.id); !purged)
        result.problems.push_back(std::format("{}: {}", game.name, purged.error().message));
    }
    if (auto done = modmanagers::Uninstall(games, *manager); !done)
      result.problems.push_back(done.error().message);
    for (const RemovalGame& purged : plan->purged) {
      if (const auto game = games.Find(purged.id))
        events.Publish("game.updated", model::ToJson(*game));
    }
  }

  std::vector<std::string> deleted = delete_data ? plan->data : std::vector<std::string>();
  if (!plan->program_dir.empty() && launcher == nullptr) deleted.push_back(plan->program_dir);
  for (const std::string& path : deleted) {
    std::error_code ec;
    fs::remove_all(path, ec);
    if (ec) result.problems.push_back(std::format("deleting {}: {}", path, ec.message()));
  }

  if (plan->signs_out) {
    const Store* store = FindStore(source);
    const Result<void> signed_out = store && store->logout ? store->logout(config) : Result<void>();
    if (!signed_out) result.problems.push_back("signing out: " + signed_out.error().message);
  }
  // Last: the games' uninstalls and the sign-out above may run them.
  for (const RemovalPlan::Tool& tool : plan->tools) {
    if (auto done = DeleteInside(tool.path, {config.File().parent_path() / "tools"}); !done)
      result.problems.push_back(std::format("deleting {}: {}", tool.name, done.error().message));
  }

  if (move_to != nullptr) {
    games.MarkSourceAdded(move_to->id, true);
    events.Publish("sources.changed", nlohmann::json::object());
  }
  store::SourceState state = games.Source(std::string(source));
  state.added = false;
  state.enabled = false;  // else Steam's next change scans it back in
  games.SaveSource(state);
  log::Info("removed source {}: {} game(s), {} problem(s)", source, result.removed,
            result.problems.size());
  return result;
}

}  // namespace mira::library
