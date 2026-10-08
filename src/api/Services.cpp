#include "core/LogHub.h"
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
#include "library/FolderTags.h"
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

// `sort_root`: the library folder (as library_roots spells it) the game is sorted in, or null when
// its folder never moves. `folder_tags`: that folder's folder tags, or null while it isn't sorted.
// `folder`: the folder tag the game's folder is in, or null when none.
void AddSorting(json& record, const library::SortRules& rules, const model::Game& game) {
  record["sort_root"] = nullptr;
  record["folder_tags"] = nullptr;
  record["folder"] = nullptr;
  const std::filesystem::path root = library::SortRootOf(rules, game);
  if (root.empty()) return;
  for (std::size_t i = 0; i < rules.roots.size(); ++i) {
    if (rules.roots[i] == root) record["sort_root"] = rules.roots_as_written[i];
  }
  if (!library::SortsByTags(rules, root)) return;
  record["folder_tags"] = library::FolderTagsOf(rules, root);
  if (const std::string folder = library::FolderTagFor(rules, root, game.tags, game.folder_tag);
      !folder.empty())
    record["folder"] = folder;
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
    if (!game.contains("art")) Decorate(game);  // a Record() already is
  });
  events.SetArtHook([this](const std::string& id) { return games.Metadata().ArtVersions(id); });
  supervisor.SetExitHook([this](const std::string& id) {
    CheckForInstall(id);
    SortByTags({id});  // its tags may have changed while it ran
  });
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

void Services::ReloadSettings() {
  auto changed = config.Reload();
  if (!changed) {
    log::Warn("{}", changed.error().message);
    events.PublishNotification(model::NotifyLevel::Error,
                               std::format("{}. {}", changed.error().message, changed.error().hint));
    return;
  }
  if (changed->empty()) return;  // Mira's own save, or an edit that changed no setting
  log::Info("settings.toml was edited: {} setting(s) changed", changed->size());
  SettingsChanged(*changed);
}

void Services::SettingsChanged(const std::vector<std::string>& keys) {
  SyncDesktopEntries();
  // Sorting folders are watched like the roots, and games move to match the new folders.
  if (std::ranges::any_of(keys, [](const std::string& key) {
        return key == "library_roots" || key == "tags.folders" || key == "tags.sorted_roots";
      })) {
    SortingChanged();
  }
  events.Publish("config.changed", {{"keys", keys}, {"frontend", config.FrontendSettings()}});
}

void Services::SyncDesktopEntries() {
  if (auto synced = desktop::DesktopEntries(config, games.Metadata()).Sync(games.All()); !synced) {
    log::Warn("could not update application menu entries: {}", synced.error().message);
  }
}

void Services::SyncDesktopEntry(const std::string& game_id) {
  if (auto synced = desktop::DesktopEntries(config, games.Metadata()).SyncOne(game_id, games.Find(game_id)); !synced) {
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

Services::RecordSettings Services::CurrentRecordSettings() const {
  return {.threshold = config.GetDouble("detect.low_confidence_threshold"), .rules = library::SortRules(config)};
}

json Services::Record(const model::Game& game, const RecordSettings* settings) {
  json record = model::ToJson(game);
  AddRecordFields(record, game, settings != nullptr ? *settings : CurrentRecordSettings());
  return record;
}

void Services::Decorate(json& record) {
  AddRecordFields(record, model::GameFromJson(record), CurrentRecordSettings());
}

void Services::AddRecordFields(json& record, const model::Game& game, const RecordSettings& settings) {
  // So a client can resync after a reconnect.
  record["running"] = supervisor.IsRunning(game.id);
  record["art"] = games.Metadata().ArtVersions(game.id);
  AddNeedsCheck(record, settings.threshold);
  AddSorting(record, settings.rules, game);
}

// Picks up games started outside Mira (the Steam client, a running launcher) so
// they show as playing and count playtime.
void Services::WatchExternalGames() {
  constexpr auto kScanEvery = std::chrono::seconds(3);
  constexpr auto kIdleScanEvery = std::chrono::seconds(15);  // no Steam or launcher games to look for
  proc::ProcessIndex index;
  auto wait = kScanEvery;
  struct Candidate {
    model::Game game;
    std::string appid;    // Steam
    std::string win_dir;  // launcher
  };
  std::vector<Candidate> candidates;
  // Rebuilt only when the library or settings change: resolving every Steam game's settings
  // each pass costs more than the /proc scan itself.
  std::optional<std::pair<std::uint64_t, std::uint64_t>> built_for;
  for (;;) {
    {
      std::unique_lock lock(stop_mutex_);
      const auto stopped = [this] { return stopping.load(std::memory_order_relaxed); };
      if (stop_wake_.wait_for(lock, wait, stopped)) return;
    }

    const std::pair revisions{games.Revision(), config.Revision()};
    if (built_for != revisions) {
      built_for = revisions;
      candidates.clear();
      for (const model::Game& game : games.All()) {
        if (game.runner_ref.starts_with("steam:")) {
          if (config::Resolver(config, game.overrides).GetBool("steam.track_process")) {
            candidates.push_back({game, game.runner_ref.substr(6), ""});
          }
        } else if (launchers::Find(game.source) && !game.data_dir.empty()) {
          candidates.push_back({game, "", launchers::TrackedPath(game)});
        }
      }
    }
    wait = candidates.empty() ? kIdleScanEvery : kScanEvery;
    if (candidates.empty()) continue;  // nothing to watch: don't touch /proc

    index.Refresh();
    std::set<std::string> steam_running;
    for (const auto& [pid, info] : index.Processes()) {
      if (!info.steam_launch.empty()) steam_running.insert(info.steam_launch);
    }
    const auto post_script = [&](const model::Game& game) {
      return config::Resolver(config, game.overrides).GetString("launch.post_script");
    };
    for (const Candidate& candidate : candidates) {
      if (supervisor.IsRunning(candidate.game.id)) continue;
      if (!candidate.appid.empty()) {
        if (!steam_running.contains(candidate.appid)) continue;
        if (supervisor.TrackSteamLaunch(candidate.game, candidate.appid,
                                        post_script(candidate.game))) {
          events.Publish("game.launched", {{"id", candidate.game.id}, {"via", "steam"}, {"tracked", true}});
        }
        continue;
      }
      const bool running = std::ranges::any_of(index.Processes(), [&](const auto& item) {
        return proc::InPrefix(item.second.prefix, candidate.game.data_dir) &&
               proc::UnderWindowsPath(item.second.argv0, candidate.win_dir);
      });
      if (running && supervisor.TrackLauncherLaunch(candidate.game, candidate.win_dir, 10,
                                                    post_script(candidate.game))) {
        events.Publish("game.launched", {{"id", candidate.game.id}, {"via", "launcher"}, {"tracked", true}});
      }
    }
  }
}

void Services::QueueMetadata(const std::vector<model::Game>& games) {
  for (const model::Game& game : games) fetches.Enqueue(config, events, game);
}

void Services::ReconcileSessions() {
  supervisor.Reconcile();
  // A client that stayed open across a restart may still show games from the old daemon as
  // running. One event, not one per game, so a large library doesn't push the whole buffer out.
  json idle = json::array();
  const RecordSettings settings = CurrentRecordSettings();
  for (const model::Game& game : games.All()) {
    if (!supervisor.IsRunning(game.id)) idle.push_back(Record(game, &settings));
  }
  if (!idle.empty()) events.Publish("games.updated", {{"games", std::move(idle)}});
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

void Services::ProvisionLater(const model::Game& game) {
  jobs.Start(
      "provision", game.id, "Setting up " + game.name,
      [this, id = game.id](JobRegistry::Progress&) -> Result<json> {
        const auto current = games.Find(id);
        if (!current) return Err("game_not_found", "the game was removed");
        const model::Game provisioned = runner::RunnerRegistry(config).ProvisionGame(*current);
        auto saved = games.Update(id, [&](model::Game& g) {
          g.runner_ref = provisioned.runner_ref;
          g.status = provisioned.status;
          g.last_error = provisioned.last_error;
        });
        if (!saved) return std::unexpected(saved.error());
        SyncDesktopEntry(id);
        json record = Record(*saved);
        events.Publish("game.updated", record);
        if (saved->status != model::GameStatus::Ready)
          return Err("provision_failed", saved->last_error);
        return record;
      },
      std::string(), &operations);
}

Result<proc::ProcessSupervisor::Reservation> Services::Claim(const std::string& game_id,
                                                             const std::string& purpose) {
  if (auto claim = supervisor.Reserve(game_id, purpose)) return std::move(*claim);
  if (supervisor.IsRunning(game_id)) return std::unexpected(GameRunningError(game_id));
  return std::unexpected(GameBusyError(game_id, supervisor.ReservedFor(game_id)));
}

void Services::SortByTags(std::vector<std::string> ids) {
  std::erase_if(ids, [&](const std::string& id) {
    const auto game = games.Find(id);
    return !game || !library::NeedsPlacing(config, *game);
  });
  if (ids.empty()) return;
  const std::string target = ids.size() == 1 ? ids.front() : std::string();
  jobs.Start("relocate", target, "Sorting game folders by tag",
             [this, ids](JobRegistry::Progress& progress) -> Result<json> {
               json moved = json::array();
               json failed = json::array();
               std::optional<Error> last_error;
               int done = 0;
               for (const std::string& id : ids) {
                 progress.Report(done++, static_cast<int>(ids.size()));
                 // Per game, so scans can run between moves.
                 auto folders_lock = games.LockFolders();
                 const auto game = games.Find(id);
                 if (!game || !library::NeedsPlacing(config, *game)) continue;  // changed meanwhile
                 const auto claim = Claim(id, "moved");
                 if (!claim) continue;
                 const std::vector<model::Game> library = games.All();
                 auto placed = library::Place(config, *game, library);
                 if (!placed) {
                   log::Warn("couldn't sort {} by its tags: {}", id, placed.error().message);
                   failed.push_back(BatchFailure(id, placed.error()));
                   last_error = placed.error();
                   continue;
                 }
                 auto saved = games.Update(id, [&](model::Game& stored) {
                   stored.install_path = placed->install_path;
                   stored.data_dir = placed->data_dir;
                   stored.library_link = placed->library_link;
                   stored.updated_at = model::NowSeconds();
                 });
                 if (!saved) {
                   log::Error("moved {} to {} but couldn't save it: {}", id, placed->install_path,
                              saved.error().message);
                   failed.push_back(BatchFailure(id, saved.error()));
                   last_error = saved.error();
                   continue;
                 }
                 library::PruneEmptyContainers(config, library::SortingFolderOf(config, *game),
                                               games.All());
                 folders_lock.unlock();
                 events.Publish("game.updated", Record(*saved));
                 moved.push_back(id);
               }
               // A single game's failure is the job's, so a client shows it.
               if (ids.size() == 1 && last_error) return std::unexpected(*last_error);
               return json{{"moved", std::move(moved)}, {"failed", std::move(failed)}};
             });
}

void Services::PublishAllGames() {
  json records = json::array();
  const RecordSettings settings = CurrentRecordSettings();
  for (const model::Game& game : games.All()) records.push_back(Record(game, &settings));
  if (!records.empty()) events.Publish("games.updated", {{"games", std::move(records)}});
}

void Services::SortingChanged() {
  if (on_roots_changed) on_roots_changed();
  PublishAllGames();
  SortAllByTags();
}

void Services::SortAllByTags() {
  std::vector<std::string> ids;
  for (const model::Game& game : games.All()) ids.push_back(game.id);
  SortByTags(std::move(ids));
}

Result<void> Services::DeleteGameData(const model::Game& game, std::span<const model::Game> staying,
                                      bool files, bool prefix, bool metadata) {
  // A desktop-entry import only links to another app's own files.
  if (game.source == "desktop-entry") files = prefix = false;
  std::vector<model::Game> others(staying.begin(), staying.end());
  std::erase_if(others, [&](const model::Game& other) { return other.id == game.id; });
  // Its link in a library folder goes with it, whatever else is kept.
  if (!game.library_link.empty()) {
    library::RemoveLink(game);
    library::PruneEmptyContainers(config, library::SortingFolderOf(config, game), others);
  }
  // Through the store's own tool where it has one, so its records stay in sync.
  if (files && game.source == "office") {
    // The apps share one folder, so Office's own installer takes just this one out.
    if (const auto host = games.Find(launchers::GameId(*launchers::Find("office")))) {
      std::vector<std::string> apps = launchers::InstalledOfficeApps(*host);
      std::erase(apps, game.source_ref);
      if (auto removed = launchers::SetOfficeApps(config, games, events, std::move(apps)); !removed) return removed;
    }
  } else if (files) {
    if (auto deleted = library::DeleteGameFiles(config, game, staying); !deleted) return deleted;
    library::PruneEmptyContainers(config, library::SortingFolderOf(config, game), others);
  }
  if (prefix && !game.data_dir.empty()) {
    const auto sharer = std::ranges::find_if(staying, [&](const model::Game& other) {
      return other.id != game.id && other.data_dir == game.data_dir;
    });
    if (sharer != staying.end()) {
      return Err("shared_prefix", std::format("\"{}\" also uses {}, so it wasn't deleted", sharer->name, game.data_dir),
                 "Remove the launcher it came with to delete its prefix.");
    }
    if (auto deleted = DeleteUnderRoot(game.data_dir, {config.GetPath("prefix_root")}); !deleted) return deleted;
  }
  if (metadata) {
    // Metadata lives in Mira's own folder, keyed by id, so no root check is needed.
    games.Metadata().Remove(game.id);
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
           [this, kind, asset, replacing, base, name](JobRegistry::Progress&) -> Result<json> {
             // Its own live log, apart from any other download running at the same time.
             const std::string channel = "runner:" + kind + ":" + name;
             loghub::Begin(channel);
             loghub::Append(channel, std::format("Downloading {} {}\n", kind, asset.tag));
             int reported = -1;
             const auto on_progress = [this, &base, &channel, &reported](double fraction) {
               json event = base;
               event["progress"] = fraction;
               events.Publish("runners.download.progress", std::move(event));
               const int tenth = static_cast<int>(fraction * 10);
               if (tenth > reported) {
                 reported = tenth;
                 loghub::Append(channel, std::format("{}%\n", tenth * 10));
               }
             };
             if (auto installed = runner::DownloadAndInstall(config, kind, asset, on_progress); !installed) {
               loghub::Append(channel, "Failed: " + installed.error().message + "\n");
               loghub::End(channel);
               log::Error("runner download failed ({} {}): {}", kind, asset.tag, installed.error().message);
               events.Publish("runners.download.failed", FailedEvent(base, installed.error()));
               return std::unexpected(installed.error());
             }
             log::Info("installed {} {}", kind, asset.tag);
             loghub::Append(channel, "Installed.\n");
             loghub::End(channel);
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
                 const RecordSettings settings = CurrentRecordSettings();
                 for (const model::Game& game : games.All()) {
                   if (game.runner_ref != replacing) continue;
                   if (auto updated = games.Update(game.id, [&](model::Game& g) { g.runner_ref = to; })) {
                     moved.push_back(Record(*updated, &settings));
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
