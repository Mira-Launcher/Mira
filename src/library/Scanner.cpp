#include "library/Scanner.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <map>
#include <mutex>
#include <set>

#include "core/Lane.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "desktop/DesktopEntries.h"
#include "library/AutoInstall.h"
#include "library/AutoSetup.h"
#include "library/ArchiveExtractor.h"
#include "library/Detector.h"
#include "library/FolderTags.h"
#include "library/WinePrefix.h"
#include "launchers/Launchers.h"
#include "runner/RunnerRegistry.h"

namespace mira::library {
namespace {
namespace fs = std::filesystem;

// What a game's status should be when its folder reappears after being
// marked Missing. Derived from the game's own data rather than assumed:
// "has an exe_path" is not the same as "launchable": an installer has one
// too, and a Windows game that was never provisioned has no prefix behind
// it. Getting this wrong silently un-did the installer guard.
model::GameStatus RestoredStatus(const model::Game& game) {
  const auto chosen = std::ranges::find(game.candidates, true, &model::Candidate::chosen);
  if (chosen != game.candidates.end() && chosen->is_installer) {
    return model::GameStatus::NeedsInstall;
  }
  if (game.exe_path.empty()) return model::GameStatus::SettingUp;
  if (game.platform == model::Platform::Native) return model::GameStatus::Ready;

  // Windows: only ready if something actually provisioned it.
  std::error_code ec;
  const bool provisioned =
      !game.runner_ref.empty() && !game.data_dir.empty() && fs::exists(game.data_dir, ec);
  return provisioned ? model::GameStatus::Ready : model::GameStatus::SettingUp;
}

// Provisions `game` if it's a Windows game still SettingUp, and writes back
// only the fields provisioning owns (see the writeback comment at the call
// site for why not a full Upsert). No-op for anything already past SettingUp.
void TryProvision(model::Game game, const runner::RunnerRegistry& runners, store::GameStore& games,
                  api::EventBus& events) {
  if (game.status != model::GameStatus::SettingUp) return;
  // Scans no longer hold the folders lock while provisioning, so two can reach the same game.
  static std::mutex provisioning_mutex;
  static std::set<std::string> provisioning;
  {
    const std::lock_guard lock(provisioning_mutex);
    if (!provisioning.insert(game.id).second) return;
  }
  const auto done = [&] {
    const std::lock_guard lock(provisioning_mutex);
    provisioning.erase(game.id);
  };
  const model::Game provisioned = runners.ProvisionGame(game);
  auto result = games.Update(game.id, [&](model::Game& stored) {
    stored.runner_ref = provisioned.runner_ref;
    stored.status = provisioned.status;
    stored.last_error = provisioned.last_error;
  });
  done();
  if (!result) {
    log::Error("failed to save provisioning result for {}: {}", game.id, result.error().message);
    return;
  }
  events.Publish("game.updated", model::ToJson(*result));
}

// Silently runs a recognized installer off the scan thread (installs can
// take minutes). A failed attempt sets last_error and isn't retried.
void QueueAutoInstall(const model::Game& game, config::Config& config, store::GameStore& games,
                      api::EventBus& events, metadata::FetchQueue* fetches, Lane* lane) {
  if (lane == nullptr || !AutoInstalls(config, game) || !BeginInstall(game.id)) return;
  lane->Post([id = game.id, &config, &games, &events, fetches] {
    [[maybe_unused]] const auto installed = RunInstall(config, games, events, fetches, id, InstallMode::kSilentOnly, std::nullopt);
  });
}

// The games whose folder is gone that `found` could be, narrowed by evidence: their program is
// inside, then every executable the detector saw for them is, then the folder has the same name.
// A step that would leave none keeps the step before.
std::vector<model::Game> MovedHere(const std::vector<model::Game>& moved_away,
                                   const fs::path& found, bool loose, const SortRules& rules) {
  std::error_code ec;
  const auto same_name = [&](const model::Game& game) {
    return fs::path(game.install_path).filename() == found.filename();
  };
  std::vector<model::Game> matches;
  for (const model::Game& game : moved_away) {
    if (IsLooseAppImage(rules, game) != loose) continue;
    // Already marked missing: likely deleted, not moved, and many games share a program name
    // (Game.exe, start.sh), so only a folder of its own name is it.
    if (game.status == model::GameStatus::Missing && !same_name(game)) continue;
    const bool program_inside =
        loose ? game.exe_path == found.filename().string()
              : !game.exe_path.empty() && fs::is_regular_file(found / game.exe_path, ec);
    if (program_inside || (!loose && game.exe_path.empty() && same_name(game)))
      matches.push_back(game);
  }
  const auto narrow = [&](const auto& keep) {
    std::vector<model::Game> kept;
    std::ranges::copy_if(matches, std::back_inserter(kept), keep);
    if (!kept.empty()) matches = std::move(kept);
  };
  if (matches.size() > 1 && !loose) {
    narrow([&](const model::Game& game) {
      return !game.candidates.empty() &&
             std::ranges::all_of(game.candidates, [&](const model::Candidate& c) {
               return fs::is_regular_file(found / c.rel_path, ec);
             });
    });
  }
  if (matches.size() > 1) narrow(same_name);
  return matches;
}

// What the library roots other than `root` hold, at their top and in their sorting folders: where
// a game whose folder is gone from `root` may have been moved by hand.
struct OtherRoots {
  std::vector<fs::path> folders;
  std::vector<fs::path> files;
};

OtherRoots ListOtherRoots(const SortRules& rules, const fs::path& root) {
  OtherRoots found;
  for (const fs::path& other : rules.roots) {
    if (other.empty() || other == RootOf(rules, root)) continue;
    std::vector<fs::path> to_list = {other};
    while (!to_list.empty()) {
      const fs::path folder = to_list.back();
      to_list.pop_back();
      std::error_code ec;
      for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
          found.files.push_back(it->path());
        } else if (it->is_directory(ec)) {
          (ContainerOf(rules, other, it->path()) ? to_list : found.folders).push_back(it->path());
        }
      }
    }
  }
  return found;
}

// Whether `game` is in `others`, moved there for that root's scan to follow.
bool MovedToAnotherRoot(const SortRules& rules, const OtherRoots& others, const model::Game& game) {
  const bool loose = IsLooseAppImage(rules, game);
  const std::vector<model::Game> one = {game};
  return std::ranges::any_of(loose ? others.files : others.folders, [&](const fs::path& path) {
    return !MovedHere(one, path, loose, rules).empty();
  });
}

}  // namespace

Result<model::Game> Scanner::Follow(const model::Game& moved, const fs::path& found, bool loose,
                                    const fs::path& root, const SortRules& rules) {
  const fs::path folder = loose ? found.parent_path() : found;
  const fs::path old_root = RootOf(rules, moved.install_path);
  const Container place =
      ContainerOf(rules, root, loose ? folder : folder.parent_path()).value_or(Container{});
  auto updated = games_.Update(moved.id, [&](model::Game& game) {
    const fs::path old_install = fs::path(game.install_path).lexically_normal();
    const fs::path relative =
        fs::path(game.data_dir).lexically_normal().lexically_relative(old_install);
    if (!loose && !game.data_dir.empty() && !relative.empty() && *relative.begin() != "..") {
      game.data_dir = (folder / relative).lexically_normal().string();
    }
    game.install_path = folder.string();
    TagsForPlace(rules, game, old_root, root, place);
    if (game.status == model::GameStatus::Missing) {
      game.status = RestoredStatus(game);
      if (game.status != model::GameStatus::NeedsInstall) game.last_error.clear();
    }
    game.updated_at = model::NowSeconds();
  });
  if (!updated) {
    log::Error("failed to follow {} to {}: {}", moved.id, folder.string(), updated.error().message);
    return std::unexpected(updated.error());
  }
  events_.Publish("game.updated", model::ToJson(*updated));
  log::Info("{} was moved by hand to {}", moved.id, found.string());
  return *updated;
}

Result<model::Game> Scanner::Settle(const fs::path& folder, const std::optional<std::string>& id) {
  auto folders_lock = games_.LockFolders();
  const SortRules rules(config_);
  const fs::path root = RootOf(rules, folder);
  std::error_code ec;
  if (root.empty() || !fs::exists(folder, ec)) {
    return Err("folder_missing",
               std::format("\"{}\" isn't in a library folder any more", folder.string()));
  }
  const bool loose = fs::is_regular_file(folder, ec);
  model::Game settled;
  if (id) {
    const auto game = games_.Find(*id);
    if (!game) return Err("game_not_found", "no such game");
    auto followed = Follow(*game, folder, loose, root, rules);
    if (!followed) return followed;
    PruneEmptyContainers(config_, SortingFolderOf(rules, *game), games_.All());
    settled = *followed;
  } else {
    AutoSetup auto_setup(config_, games_, events_);
    settled =
        loose ? auto_setup.CreateAppImageGame(folder.parent_path(), folder)
              : auto_setup.CreateGame(folder, Detector(SettingsFromConfig(config_)).Detect(folder));
  }
  // The other games it could have been were spared by the missing pass while it waited: their
  // roots are scanned again, so each is marked missing or found where it went.
  std::set<fs::path> rescan;
  if (unclear_moves_ != nullptr) {
    if (const auto move = unclear_moves_->Find(folder)) {
      for (const std::string& other : move->ids) {
        const auto game = other == settled.id ? std::nullopt : games_.Find(other);
        if (game && !RootOf(rules, game->install_path).empty()) rescan.insert(RootOf(rules, game->install_path));
      }
    }
    unclear_moves_->Settle(folder);
  }
  folders_lock.unlock();
  for (const fs::path& other_root : rescan) ScanRoot(other_root);
  return settled;
}

Scanner::Scanner(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

ScanSummary Scanner::ScanAll() {
  ScanSummary total;
  for (const fs::path& root : config_.GetPathArray("library_roots")) {
    ScanSummary partial = ScanRoot(root);
    total.added += partial.added;
    total.missing += partial.missing;
    total.restored += partial.restored;
    total.moved += partial.moved;
    std::ranges::move(partial.added_games, std::back_inserter(total.added_games));
    std::ranges::move(partial.unclear, std::back_inserter(total.unclear));
  }
  if (config_.GetBool("launchers.auto_import")) {
    library::ImportSummary imported = launchers::ImportAll(config_, games_, events_);
    total.added += imported.added;
    std::ranges::move(imported.added_games, std::back_inserter(total.added_games));
  }
  if (config_.GetBool("auto_setup")) RetryBrokenProvisioning(config_, games_, events_);
  return total;
}

ScanSummary Scanner::ScanRoot(const fs::path& root) {
  auto folders_lock = games_.LockFolders();
  ScanSummary summary;
  std::error_code ec;
  if (!fs::is_directory(root, ec)) {
    log::Warn("library root {} does not exist, skipping", root.string());
    return summary;
  }

  const fs::path prefix_root = config_.GetPath("prefix_root");
  // Sources with their own structured tracking (gog-<id>/itch-<id> rows,
  // via GogImporter/ItchImporter) shouldn't also get double-detected here
  // as one big bogus game named after the wrapper folder itself.
  const std::vector<fs::path> excluded_roots = {prefix_root, config_.GetPath("epic.install_root"),
                                                config_.GetPath("gog.install_root"),
                                                config_.GetPath("itch.install_root"),
                                                config_.GetPath("amazon.install_root")};
  const DetectorSettings detector_settings = SettingsFromConfig(config_);
  const Detector detector(detector_settings);
  AutoSetup auto_setup(config_, games_, events_);
  const bool setup = config_.GetBool("auto_setup");

  std::set<std::string> seen_install_paths;
  std::vector<std::string> to_set_up;  // provisioned once the folders lock is released
  // An installer's folder whose game now lives elsewhere (usually in its prefix) isn't a new game.
  std::set<std::string> installer_dirs;
  const SortRules rules(config_);
  const auto IsLooseFile = [&](const model::Game& game) { return IsLooseAppImage(rules, game); };
  // Games run from a file loose in a folder (the root, or a sorting folder in it), by folder and
  // file name.
  std::map<std::pair<fs::path, std::string>, model::Game> loose_programs;
  // Games whose folder is gone from any library root: a new folder may be one of them, moved by
  // hand.
  std::vector<model::Game> moved_away;
  // Games sorted by a link to their folder, by that folder: a link to one of them is Mira's.
  std::map<fs::path, model::Game> linked_by_target;
  for (const model::Game& game : games_.All()) {
    if (!game.installer_dir.empty()) installer_dirs.insert(game.installer_dir);
    if (SortsByLink(rules, game)) linked_by_target.emplace(fs::path(game.install_path).lexically_normal(), game);
    if (IsLooseFile(game))
      loose_programs.emplace(std::pair{fs::path(game.install_path), game.exe_path}, game);
    // A game in a root that isn't there (an unplugged drive) hasn't moved anywhere.
    const fs::path game_root =
        game.install_path.empty() ? fs::path() : RootOf(rules, game.install_path);
    if ((game.source == "scan" || game.source == "manual") && !game_root.empty() &&
        fs::is_directory(game_root, ec) &&
        !fs::exists(IsLooseFile(game) ? fs::path(game.install_path) / game.exe_path
                                      : fs::path(game.install_path),
                    ec)) {
      moved_away.push_back(game);
    }
  }

  // An AppImage is a whole game in one file, so one loose in a folder is a game too.
  std::set<std::pair<fs::path, std::string>> seen_appimages;  // folder and file name
  std::vector<fs::path> left_folders;  // where adopted games were, pruned once the pass is done

  // Folders that could be any of several games, and those games, which aren't marked missing.
  std::vector<UnclearMove> unclear;
  std::set<std::string> unclear_ids;

  // A known game moved here by hand: it keeps its id, history and settings, and its tags follow
  // the new place. Several possible games make it unclear, for someone to settle.
  const auto adopt = [&](const fs::path& found, bool loose) -> bool {
    const std::vector<model::Game> matches = MovedHere(moved_away, found, loose, rules);
    if (matches.empty()) return false;
    if (matches.size() > 1) {
      UnclearMove move{.folder = found, .ids = {}};
      for (const model::Game& game : matches) {
        move.ids.push_back(game.id);
        unclear_ids.insert(game.id);
      }
      unclear.push_back(std::move(move));
      log::Info("{} could be any of {} games moved by hand; waiting to be told which",
                found.string(), matches.size());
      return true;
    }
    const model::Game moved = matches.front();
    if (!Follow(moved, found, loose, root, rules)) return false;
    left_folders.push_back(SortingFolderOf(rules, moved));
    std::erase_if(moved_away, [&](const model::Game& game) { return game.id == moved.id; });
    ++summary.moved;
    return true;
  };

  // A link to a game sorted by link is Mira's, never a game or a folder to look in; one moved by
  // hand makes the game's tags follow it. False for any other link, which is scanned as a folder.
  const auto follow_link = [&](const fs::path& link, const fs::path& folder) -> bool {
    std::error_code link_ec;
    const fs::path target = fs::read_symlink(link, link_ec);
    if (link_ec) return false;
    const auto found = linked_by_target.find(target.lexically_normal());
    if (found == linked_by_target.end()) return false;
    const model::Game& game = found->second;
    if (fs::path(game.library_link).lexically_normal() == link.lexically_normal()) return true;
    // Its own link still in place: this one is a copy, left alone, or the two would swap places
    // (and its tags) on every scan.
    if (!game.library_link.empty() &&
        fs::is_symlink(fs::symlink_status(game.library_link, link_ec)) &&
        fs::read_symlink(game.library_link, link_ec).lexically_normal() ==
            target.lexically_normal()) {
      return true;
    }
    const fs::path old_root = game.library_link.empty() ? root : RootOf(rules, game.library_link);
    const Container place = ContainerOf(rules, root, folder).value_or(Container{});
    auto updated = games_.Update(game.id, [&](model::Game& stored) {
      stored.library_link = link.string();
      TagsForPlace(rules, stored, old_root, root, place);
      stored.updated_at = model::NowSeconds();
    });
    if (!updated) return true;
    if (!game.library_link.empty()) left_folders.push_back(SortingFolderOf(rules, game));
    found->second = *updated;
    events_.Publish("game.updated", model::ToJson(*updated));
    ++summary.moved;
    log::Info("{}'s link was moved by hand to {}", game.id, link.string());
    return true;
  };

  // The root and the sorting folders found in it (.hidden, a folder tag's folder): the missing pass
  // checks only games in folders listed here.
  std::set<fs::path> listed;
  std::vector<fs::path> to_list = {root};
  // A listing that fails (unreadable root, stale network mount) must not look like an empty root,
  // or the missing pass below would mark or remove every game in it. So no skip_permission_denied,
  // which turns an unreadable root into an empty one.
  std::error_code list_ec;
  while (!to_list.empty()) {
    const fs::path folder = to_list.back();
    to_list.pop_back();
    std::error_code folder_ec;
    std::vector<fs::directory_entry> entries;
    for (fs::directory_iterator it(folder, folder_ec), end; !folder_ec && it != end;
         it.increment(folder_ec)) {
      entries.push_back(*it);
    }
    if (folder_ec) {
      if (folder == root) {
        list_ec = folder_ec;
        break;
      }
      log::Warn("could not list {}: {}; not checking it for missing games", folder.string(),
                folder_ec.message());
      continue;
    }
    listed.insert(folder);
    for (const fs::directory_entry& entry : entries) {
      if (entry.is_symlink(ec) && entry.is_directory(ec) && follow_link(entry.path(), folder)) continue;
      if (entry.is_regular_file(ec) &&
          strings::ToLower(entry.path().extension().string()) == ".appimage") {
        const std::string file = entry.path().filename().string();
        seen_appimages.emplace(folder, file);
        const auto known = loose_programs.find({folder, file});
        if (known == loose_programs.end()) {
          if (adopt(entry.path(), /*loose=*/true)) continue;
          const model::Game game = auto_setup.CreateAppImageGame(folder, entry.path());
          ++summary.added;
          summary.added_games.push_back(game);
          log::Info("detected new game: {}", entry.path().string());
        } else if (known->second.status == model::GameStatus::Missing) {
          if (auto restored = games_.Update(known->second.id, [](model::Game& game) {
                game.status = model::GameStatus::Ready;
              })) {
            events_.Publish("game.updated", model::ToJson(*restored));
            ++summary.restored;
          }
        }
        continue;
      }
      if (!entry.is_directory(ec)) continue;
      const fs::path& dir = entry.path();
      if (dir.filename().string().starts_with(kExtractingPrefix))
        continue;  // an archive mid-extraction

      const bool excluded = std::ranges::any_of(excluded_roots, [&](const fs::path& excluded_root) {
        std::error_code eq;
        return fs::equivalent(dir, excluded_root, eq) || (!eq && dir == excluded_root);
      });
      if (excluded) continue;

      const std::string install_path = dir.string();
      auto existing = games_.FindByInstallPath(install_path);
      // A sorting folder holds games rather than being one; a known game of the same name stays a
      // game.
      if (!existing && ContainerOf(rules, root, dir)) {
        to_list.push_back(dir);
        continue;
      }
      if (!existing && installer_dirs.contains(install_path)) continue;
      // A known game runs from a subfolder of it (Binaries/, bin/): not a new game.
      if (!existing && games_.HasInstallUnder(install_path)) continue;

      // A combined install+prefix layout (Lutris colocates a Wine prefix
      // inside the game's own folder) legitimately looks like a Wine prefix
      // too. Only exclude that shape from *new* detection, never from a
      // folder that's already a known game, or every scan would flip it to
      // Missing.
      if (!existing && LooksLikeWinePrefix(dir)) continue;

      const std::string basename = dir.filename().string();
      const bool ignored =
          std::ranges::any_of(detector_settings.ignore_globs, [&](const std::string& glob) {
            return strings::GlobMatch(strings::ToLower(glob), strings::ToLower(basename));
          });
      if (ignored) continue;

      seen_install_paths.insert(install_path);

      if (existing) {
        if (existing->status == model::GameStatus::Missing) {
          auto result = games_.Update(existing->id, [](model::Game& game) {
            game.status = RestoredStatus(game);
            if (game.status != model::GameStatus::NeedsInstall) game.last_error.clear();
          });
          if (!result) {
            log::Error("failed to restore {}: {}", existing->id, result.error().message);
          } else {
            existing = *result;
            // Same reasoning as game.removed below: a listener that already
            // has this game (now Missing) needs to hear about it coming back,
            // without waiting on a caller to relist the whole library.
            events_.Publish("game.updated", model::ToJson(*existing));
          }
          ++summary.restored;
        }
        // Retry provisioning for a game still stuck at SettingUp: auto_setup
        // may have been off when it was first detected and turned on since, a
        // previous attempt may have failed transiently, or the daemon may have
        // restarted mid-provision last time. Without this, SettingUp is a dead
        // end reachable only by the one provisioning attempt at detection time.
        if (setup) to_set_up.push_back(existing->id);
        continue;  // already known; never re-detect over a user's configuration
      }

      if (adopt(dir, /*loose=*/false)) continue;

      const Detector::Result detected = detector.Detect(dir);
      const model::Game game = auto_setup.CreateGame(dir, detected);
      ++summary.added;
      summary.added_games.push_back(game);
      log::Info("detected new game: {}", install_path);

      // With auto_setup off, a game is still detected and stored (so it shows
      // up for the frontend to configure) but never auto-provisioned.
      if (setup) to_set_up.push_back(game.id);
    }
  }

  // Anything previously known under this root but not seen this pass has
  // disappeared. Default: mark it, don't delete it, so configuration and
  // playtime survive an unplugged drive or a temporarily-offline network
  // share. `library.remove_missing` trades that safety net for an
  // always-current list. It still never touches the game's files themselves,
  // same as DELETE /v1/games/{id}.
  const bool remove_missing = config_.GetBool("library.remove_missing");
  if (list_ec)
    log::Warn("could not list library root {}: {}; not checking for missing games", root.string(),
              list_ec.message());
  // A folder this pass listed, or one in this root that's gone (a sorting folder deleted with its
  // games in it). A folder that couldn't be read is neither, so its games are left alone.
  const fs::path this_root = RootOf(rules, root);
  const auto checked = [&](const fs::path& folder) {
    if (listed.contains(folder)) return true;
    if (this_root.empty() || RootOf(rules, folder) != this_root) return false;
    std::error_code gone_ec;
    return !fs::exists(folder, gone_ec) && !gone_ec;
  };
  std::optional<OtherRoots> other_roots;
  for (const model::Game& game : list_ec ? std::vector<model::Game>{} : games_.All()) {
    if (unclear_ids.contains(game.id)) continue;  // may be the folder waiting to be settled
    if (IsLooseFile(game)) {
      if (!checked(fs::path(game.install_path))) continue;
      if (seen_appimages.contains({fs::path(game.install_path), game.exe_path})) continue;
    } else {
      if (!checked(fs::path(game.install_path).parent_path())) continue;
      if (seen_install_paths.contains(game.install_path)) continue;
    }

    // Checked before the already-Missing skip below, not after: otherwise
    // turning the toggle on would only ever catch a game the *next* time it
    // disappears, leaving anything already sitting at Missing stuck there
    // forever, which is surprising, since the whole point of flipping it on is to
    // clean up what's already gone.
    // Moved by hand into another root: only marked, so that root's scan follows it with its history.
    if (remove_missing && std::ranges::contains(moved_away, game.id, &model::Game::id)) {
      if (!other_roots) other_roots = ListOtherRoots(rules, root);  // listed once, if ever needed
    }
    if (remove_missing && !(std::ranges::contains(moved_away, game.id, &model::Game::id) &&
                            MovedToAnotherRoot(rules, *other_roots, game))) {
      auto removed = games_.Remove(game.id);
      if (!removed) {
        log::Error("failed to remove missing game {}: {}", game.id, removed.error().message);
        continue;
      }
      events_.Publish("game.removed", {{"id", game.id}});
      ++summary.missing;
      log::Info("game folder disappeared, removing (library.remove_missing): {}", game.install_path);
      continue;
    }

    if (game.status == model::GameStatus::Missing) continue;
    auto result = games_.Update(game.id, [](model::Game& g) { g.status = model::GameStatus::Missing; });
    if (!result) {
      log::Error("failed to mark {} missing: {}", game.id, result.error().message);
    } else {
      // Same reasoning as the restore/remove-missing cases above: a listener
      // needs to hear about this without a caller having to relist.
      events_.Publish("game.updated", model::ToJson(*result));
    }
    ++summary.missing;
    log::Info("game folder disappeared, marking missing: {}", game.install_path);
  }

  if (!left_folders.empty()) {
    const std::vector<model::Game> library = games_.All();
    for (const fs::path& left : left_folders) PruneEmptyContainers(config_, left, library);
  }
  if (!list_ec && unclear_moves_ != nullptr) unclear_moves_->Replace(root, unclear, games_);
  summary.unclear = std::move(unclear);

  // Provisioning can take minutes (umu downloads its runtime on first use), so scans, the watcher, relocation
  // and source removal don't wait for it.
  folders_lock.unlock();
  const runner::RunnerRegistry runners(config_);
  for (const std::string& id : to_set_up) {
    auto game = games_.Find(id);
    if (!game) continue;
    TryProvision(*game, runners, games_, events_);
    game = games_.Find(id);
    if (game) QueueAutoInstall(*game, config_, games_, events_, metadata_fetches_, installs_);
  }

  // The application menu follows the library: a game that just became
  // launchable gains an entry, one that vanished loses it.
  if (auto synced = desktop::DesktopEntries(config_, games_.Metadata()).Sync(games_.All()); !synced) {
    log::Warn("could not update application menu entries: {}", synced.error().message);
  }

  return summary;
}

namespace {

// Provisions a broken Windows game again if a runner now resolves for it.
bool Reprovision(const runner::RunnerRegistry& runners, store::GameStore& games, api::EventBus& events,
                 const model::Game& game) {
  if (game.status != model::GameStatus::Broken || game.platform != model::Platform::Windows) return false;
  // A broken game with no executable on disk broke for another reason.
  std::error_code ec;
  if (game.exe_path.empty() || !fs::is_regular_file(fs::path(game.install_path) / game.exe_path, ec)) return false;
  if (!runners.Resolve(runners.ResolveRef(game))) return false;

  const model::Game provisioned = runners.ProvisionGame(game);
  auto result = games.Update(game.id, [&](model::Game& stored) {
    stored.runner_ref = provisioned.runner_ref;
    stored.status = provisioned.status;
    stored.last_error = provisioned.last_error;
  });
  if (!result) return false;
  events.Publish("game.updated", model::ToJson(*result));
  return result->status == model::GameStatus::Ready;
}

}  // namespace

int RetryBrokenProvisioning(config::Config& config, store::GameStore& games, api::EventBus& events) {
  const runner::RunnerRegistry runners(config);
  int fixed = 0;
  for (const model::Game& game : games.All()) {
    if (Reprovision(runners, games, events, game)) ++fixed;
  }
  if (fixed > 0) (void)desktop::DesktopEntries(config, games.Metadata()).Sync(games.All());
  return fixed;
}

bool RetryBrokenProvisioning(config::Config& config, store::GameStore& games, api::EventBus& events,
                             const std::string& id) {
  const std::optional<model::Game> game = games.Find(id);
  if (!game || !Reprovision(runner::RunnerRegistry(config), games, events, *game)) return false;
  (void)desktop::DesktopEntries(config, games.Metadata()).Sync(games.All());
  return true;
}

}  // namespace mira::library
