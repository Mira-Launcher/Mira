#include "library/Scanner.h"

#include <algorithm>
#include <format>
#include <iterator>
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

}  // namespace

Scanner::Scanner(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

ScanSummary Scanner::ScanAll() {
  ScanSummary total;
  for (const fs::path& root : config_.GetPathArray("library_roots")) {
    ScanSummary partial = ScanRoot(root);
    total.added += partial.added;
    total.missing += partial.missing;
    total.restored += partial.restored;
    std::ranges::move(partial.added_games, std::back_inserter(total.added_games));
  }
  if (config_.GetBool("launchers.auto_import")) {
    launchers::ImportSummary imported = launchers::ImportAll(config_, games_, events_);
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
  const std::vector<fs::path> excluded_roots = {prefix_root, config_.GetPath("gog.install_root"),
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
  for (const model::Game& game : games_.All()) {
    if (!game.installer_dir.empty()) installer_dirs.insert(game.installer_dir);
  }

  for (const auto& entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
    if (!entry.is_directory(ec)) continue;
    const fs::path& dir = entry.path();
    if (dir.filename().string().starts_with(kExtractingPrefix)) continue;  // an archive mid-extraction

    const bool excluded = std::ranges::any_of(excluded_roots, [&](const fs::path& excluded_root) {
      std::error_code eq;
      return fs::equivalent(dir, excluded_root, eq) || (!eq && dir == excluded_root);
    });
    if (excluded) continue;

    const std::string install_path = dir.string();
    auto existing = games_.FindByInstallPath(install_path);
    if (!existing && installer_dirs.contains(install_path)) continue;

    // A combined install+prefix layout (Lutris colocates a Wine prefix
    // inside the game's own folder) legitimately looks like a Wine prefix
    // too. Only exclude that shape from *new* detection, never from a
    // folder that's already a known game, or every scan would flip it to
    // Missing.
    if (!existing && LooksLikeWinePrefix(dir)) continue;

    const std::string basename = dir.filename().string();
    const bool ignored = std::ranges::any_of(detector_settings.ignore_globs, [&](const std::string& glob) {
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

    const Detector::Result detected = detector.Detect(dir);
    const model::Game game = auto_setup.CreateGame(dir, detected);
    ++summary.added;
    summary.added_games.push_back(game);
    log::Info("detected new game: {}", install_path);

    // With auto_setup off, a game is still detected and stored (so it shows
    // up for the frontend to configure) but never auto-provisioned.
    if (setup) to_set_up.push_back(game.id);
  }

  // Anything previously known under this root but not seen this pass has
  // disappeared. Default: mark it, don't delete it, so configuration and
  // playtime survive an unplugged drive or a temporarily-offline network
  // share. `library.remove_missing` trades that safety net for an
  // always-current list. It still never touches the game's files themselves,
  // same as DELETE /v1/games/{id}.
  const bool remove_missing = config_.GetBool("library.remove_missing");
  for (const model::Game& game : games_.All()) {
    if (fs::path(game.install_path).parent_path() != root) continue;
    if (seen_install_paths.contains(game.install_path)) continue;

    // Checked before the already-Missing skip below, not after: otherwise
    // turning the toggle on would only ever catch a game the *next* time it
    // disappears, leaving anything already sitting at Missing stuck there
    // forever, which is surprising, since the whole point of flipping it on is to
    // clean up what's already gone.
    if (remove_missing) {
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
  if (auto synced = desktop::DesktopEntries(config_).Sync(games_.All()); !synced) {
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
  if (fixed > 0) (void)desktop::DesktopEntries(config).Sync(games.All());
  return fixed;
}

bool RetryBrokenProvisioning(config::Config& config, store::GameStore& games, api::EventBus& events,
                             const std::string& id) {
  const std::optional<model::Game> game = games.Find(id);
  if (!game || !Reprovision(runner::RunnerRegistry(config), games, events, *game)) return false;
  (void)desktop::DesktopEntries(config).Sync(games.All());
  return true;
}

}  // namespace mira::library
