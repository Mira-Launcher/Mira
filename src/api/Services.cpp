#include "api/Services.h"

#include <chrono>
#include <condition_variable>
#include <format>
#include <memory>
#include <mutex>

#include <httplib.h>

#include "api/Http.h"
#include "config/Resolver.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "desktop/DesktopEntries.h"
#include "epic/Legendary.h"
#include "launchers/Launchers.h"
#include "library/AutoInstall.h"
#include "library/Scanner.h"
#include "library/SourceRemoval.h"
#include "metadata/MetadataFetcher.h"
#include "proc/ProcessIndex.h"
#include "runner/RunnerRegistry.h"
#include "runner/RunnerUpdates.h"

namespace mira::api {
namespace {
using nlohmann::json;

// `needs_check`: Mira picked the executable itself, wasn't sure, and nobody has confirmed it yet.
void AddNeedsCheck(json& game, double threshold) {
  const json candidates = game.value("candidates", json::array());
  game["needs_check"] = candidates.is_array() && !candidates.empty() && !game.value("reviewed", false) &&
                        game.value("confidence", 1.0) < threshold;
}

// Deletes `target` only if it resolves (symlinks included) inside one of `roots`.
Result<void> DeleteUnderRoot(const std::string& target, const std::vector<std::filesystem::path>& roots) {
  return library::DeleteInside(target, roots);
}

}  // namespace

Services::Services(config::Config& config_in, store::GameStore& games_in, EventBus& events_in)
    : config(config_in),
      games(games_in),
      events(events_in),
      supervisor(games_in, events_in, config_in.GetInt("launch.stop_timeout_s")) {
  // Importers and the scanner publish bare records; this gives every game
  // event the same `running` and `art` as GET /v1/games.
  events.SetGameRecordHook([this](json& game) {
    const std::string id = game.value("id", "");
    game["running"] = supervisor.IsRunning(id);
    game["art"] = art_index.For(id);
    AddNeedsCheck(game, config.GetDouble("detect.low_confidence_threshold"));
  });
  events.SetArtHook([this](const std::string& id) { return art_index.For(id); });
  supervisor.SetExitHook([this](const std::string& id) { CheckForInstall(id); });
}

Services::~Services() {
  events.SetGameRecordHook(nullptr);
  events.SetArtHook(nullptr);
  BeginStopping();
  if (external_watch_.joinable()) external_watch_.join();
  // Their tasks report to `jobs`, which is destroyed before the lanes are.
  for (Lane* lane : {&tricks, &artwork_selects, &artwork_thumbs, &operations, &installs}) lane->Stop();
}

void Services::BeginStopping() {
  {
    // Under the lock, so the watcher can't check the flag and then miss the wake.
    const std::lock_guard lock(stop_mutex_);
    stopping.store(true, std::memory_order_relaxed);
  }
  stop_wake_.notify_all();
  events.WakeWaiters();  // open event streams
}

void Services::StartExternalWatch() { external_watch_ = std::thread(&Services::WatchExternalGames, this); }

void Services::SyncDesktopEntries() {
  if (auto synced = desktop::DesktopEntries(config).Sync(games.All()); !synced) {
    log::Warn("could not update application menu entries: {}", synced.error().message);
  }
}

void Services::SyncDesktopEntry(const std::string& game_id) {
  if (auto synced = desktop::DesktopEntries(config).SyncOne(game_id, games.Find(game_id)); !synced) {
    log::Warn("could not update the application menu entry of {}: {}", game_id, synced.error().message);
  }
}

void Services::AfterImport(const std::vector<model::Game>& added) {
  SyncDesktopEntries();
  QueueMetadata(added);
}

void Services::WatchForInstall(const std::string& game_id, std::set<std::filesystem::path> folders) {
  const std::lock_guard lock(install_watch_mutex_);
  install_watch_[game_id] = std::move(folders);
}

void Services::CheckForInstall(const std::string& game_id) {
  std::set<std::filesystem::path> before;
  {
    const std::lock_guard lock(install_watch_mutex_);
    auto watched = install_watch_.extract(game_id);
    if (watched.empty()) return;
    before = std::move(watched.mapped());
  }
  const auto game = games.Find(game_id);
  if (!game) return;
  const auto installed = library::NewInstall(config, game->data_dir, before);
  if (!installed) return;
  log::Info("{} installed {} when run; asking whether to use it", game_id, installed->dir.string());
  events.Publish("game.install_detected",
                  {{"id", game_id}, {"install_path", installed->dir.string()}, {"exe_path", installed->exe_path}});
}

json Services::Record(const model::Game& game) {
  json body = model::ToJson(game);
  // So a client can resync after a reconnect.
  body["running"] = supervisor.IsRunning(game.id);
  body["art"] = art_index.For(game.id);
  AddNeedsCheck(body, config.GetDouble("detect.low_confidence_threshold"));
  return body;
}

// Picks up games started outside Mira (the Steam client, a running launcher) so
// they show as playing and count playtime.
void Services::WatchExternalGames() {
  constexpr auto kScanEvery = std::chrono::seconds(3);
  constexpr auto kIdleScanEvery = std::chrono::seconds(15);  // no Steam or launcher games to look for
  proc::ProcessIndex index;
  auto wait = kScanEvery;
  for (;;) {
    {
      std::unique_lock lock(stop_mutex_);
      const auto stopped = [this] { return stopping.load(std::memory_order_relaxed); };
      if (stop_wake_.wait_for(lock, wait, stopped)) return;
    }

    struct Candidate {
      model::Game game;
      std::string appid;    // Steam
      std::string win_dir;  // launcher
    };
    std::vector<Candidate> candidates;
    for (const model::Game& game : games.All()) {
      if (supervisor.IsRunning(game.id)) continue;
      if (game.runner_ref.starts_with("steam:")) {
        if (config::Resolver(config, game.overrides).GetBool("steam.track_process")) {
          candidates.push_back({game, game.runner_ref.substr(6), ""});
        }
      } else if (launchers::Find(game.source) && !game.data_dir.empty()) {
        candidates.push_back({game, "", launchers::WindowsDir(game)});
      }
    }
    wait = candidates.empty() ? kIdleScanEvery : kScanEvery;
    if (candidates.empty()) continue;  // nothing to watch: don't touch /proc

    index.Refresh();
    std::set<std::string> steam_running;
    for (const auto& [pid, info] : index.Processes()) {
      if (!info.steam_launch.empty()) steam_running.insert(info.steam_launch);
    }
    for (const Candidate& candidate : candidates) {
      const std::string post_script =
          config::Resolver(config, candidate.game.overrides).GetString("launch.post_script");
      if (!candidate.appid.empty()) {
        if (!steam_running.contains(candidate.appid)) continue;
        if (supervisor.TrackSteamLaunch(candidate.game, candidate.appid, post_script)) {
          events.Publish("game.launched", {{"id", candidate.game.id}, {"via", "steam"}, {"tracked", true}});
        }
        continue;
      }
      const bool running = std::ranges::any_of(index.Processes(), [&](const auto& item) {
        return proc::InPrefix(item.second.prefix, candidate.game.data_dir) &&
               item.second.argv0.starts_with(candidate.win_dir + "/");
      });
      if (running && supervisor.TrackLauncherLaunch(candidate.game, candidate.win_dir, 10, post_script)) {
        events.Publish("game.launched", {{"id", candidate.game.id}, {"via", "launcher"}, {"tracked", true}});
      }
    }
  }
}

void Services::QueueMetadata(const std::vector<model::Game>& games) {
  for (const model::Game& game : games) fetches.Enqueue(config, events, game);
}

void Services::ReconcileSessions() {
  supervisor.Reconcile(games.Dir() / "sessions");
  // A client that stayed open across a restart may still show games from
  // the old daemon as running.
  for (const model::Game& game : games.All()) {
    if (supervisor.IsRunning(game.id)) continue;
    json event = Record(game);
    event["state"] = "idle";
    events.Publish("game.state", std::move(event));
  }
}

void Services::StartJob(const httplib::Request& req, httplib::Response& res, const std::string& kind, const std::string& target,
                      const std::string& label, JobRegistry::Work work, Lane* lane) {
  const std::string id = jobs.Start(kind, target, label, std::move(work), req.get_param_value("job"), lane);
  SendJson(res, {{"status", "running"}, {"job", id}}, 202);
}

Result<json> Services::RefreshMetadata(std::vector<model::Game> games, JobRegistry::Progress& progress) {
  struct Tally {
    std::mutex mutex;
    std::condition_variable changed;
    int done = 0;
    int failed = 0;
  };
  const auto tally = std::make_shared<Tally>();
  const int total = static_cast<int>(games.size());
  progress.Report(0, total);
  for (model::Game& game : games) {
    fetches.Enqueue(config, events, std::move(game), /*force=*/true, /*announce=*/false,
                              [tally](bool ok) {
                                std::lock_guard lock(tally->mutex);
                                ++tally->done;
                                if (!ok) ++tally->failed;
                                tally->changed.notify_all();
                              });
  }
  std::unique_lock lock(tally->mutex);
  int reported = 0;
  while (tally->done < total) {
    // Polled, so quitting mid-refresh doesn't wait out every fetch still queued.
    if (stopping.load(std::memory_order_relaxed)) {
      return Err("shutting_down", "mirad stopped before the refresh finished");
    }
    tally->changed.wait_for(lock, std::chrono::milliseconds(250));
    if (tally->done == reported) continue;
    reported = tally->done;
    lock.unlock();
    progress.Report(reported, total);
    lock.lock();
  }
  return json{{"refreshed", total - tally->failed}, {"failed", tally->failed}};
}

Result<void> Services::DeleteGameData(const model::Game& game, bool files, bool prefix, bool metadata) {
  if (supervisor.IsRunning(game.id)) return std::unexpected(GameRunningError(game.id));
  // A desktop-entry import only links to another app's own files.
  if (game.source == "desktop-entry") files = prefix = false;
  // Through the store's own tool where it has one, so its records stay in sync.
  if (files) {
    if (auto deleted = library::DeleteGameFiles(config, game, games.All()); !deleted) return deleted;
  }
  if (prefix) {
    if (auto deleted = DeleteUnderRoot(game.data_dir, {config.GetPath("prefix_root")}); !deleted) return deleted;
  }
  if (metadata) {
    // Metadata lives in Mira's own folder, keyed by id, so no root check is needed.
    std::error_code ec;
    std::filesystem::remove(metadata::MetadataFile(config, game.id), ec);
    if (ec) log::Warn("could not remove metadata for {}: {}", game.id, ec.message());
    std::filesystem::remove_all(metadata::ArtworkDir(config, game.id), ec);
    if (ec) log::Warn("could not remove artwork for {}: {}", game.id, ec.message());
  }
  return {};
}

void Services::InstallRunner(const httplib::Request& req, httplib::Response& res, const std::string& kind,
                             const std::string& source, const runner::ReleaseAsset& asset,
                             const std::string& replacing) {
  const std::string name = runner::ReleaseName(kind, asset);
  const json base = {{"kind", kind}, {"tag", asset.tag}, {"name", name},
                     {"label", runner::BuildLabel(kind, name)}, {"source", source}};
  events.Publish("runners.download.started", base);
  StartJob(req, res, "runner", kind + ":" + name, "Downloading " + runner::BuildLabel(kind, name),
           [this, kind, asset, replacing, base](JobRegistry::Progress&) -> Result<json> {
             const auto on_progress = [this, &base](double fraction) {
               json event = base;
               event["progress"] = fraction;
               events.Publish("runners.download.progress", std::move(event));
             };
             if (auto installed = runner::DownloadAndInstall(config, kind, asset, on_progress); !installed) {
               log::Error("runner download failed ({} {}): {}", kind, asset.tag, installed.error().message);
               events.Publish("runners.download.failed", FailedEvent(base, installed.error()));
               return std::unexpected(installed.error());
             }
             log::Info("installed {} {}", kind, asset.tag);
             library::RetryBrokenProvisioning(config, games, events);
             json finished = base;
             if (!replacing.empty()) {
               // Move what used the old build onto the new one.
               const runner::RunnerRegistry registry(config);
               const std::vector<model::RunnerBuild> builds = runner::BuildsOfKind(registry, kind);
               const auto fresh = std::ranges::find_if(builds, [&](const model::RunnerBuild& build) {
                 return runner::IsInstalledAs(kind, build.release, runner::BuildDir(build).filename().string(), asset);
               });
               if (fresh != builds.end()) {
                 const std::string to = fresh->Reference();
                 json moved = json::array();
                 const auto batch = games.BatchSaves();
                 for (const model::Game& game : games.All()) {
                   if (game.runner_ref != replacing) continue;
                   if (auto updated = games.Update(game.id, [&](model::Game& g) { g.runner_ref = to; })) {
                     moved.push_back(Record(*updated));
                   }
                 }
                 if (config.GetString("default_runner.windows") == replacing) {
                   (void)config.Set("default_runner.windows", to);
                 }
                 if (!moved.empty()) events.Publish("games.updated", {{"games", moved}});
                 events.Publish("runners.updated",
                                {{"kind", kind}, {"from", replacing}, {"to", to}, {"games", moved.size()}});
                 finished["replaced"] = replacing;
               }
             }
             events.Publish("runners.download.finished", finished);
             return finished;
           },
           &operations);
}

}  // namespace mira::api
