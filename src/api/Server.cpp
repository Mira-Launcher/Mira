#include "api/Server.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include <httplib.h>

#include "config/Resolver.h"
#include "config/Schema.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "desktop/DesktopEntries.h"
#include "library/Catalog.h"
#include "library/AutoInstall.h"
#include "library/Detector.h"
#include "library/PrefixNaming.h"
#include "library/Relocate.h"
#include "library/Scanner.h"
#include "library/SourceRegistry.h"
#include "amazon/AmazonImporter.h"
#include "amazon/Nile.h"
#include "desktop/DesktopEntryScanner.h"
#include "epic/EpicImporter.h"
#include "epic/EpicInstaller.h"
#include "epic/Legendary.h"
#include "gog/Gog.h"
#include "gog/GogImporter.h"
#include "gog/GogInstaller.h"
#include "humble/Humble.h"
#include "itch/Itch.h"
#include "library/SourceRemoval.h"
#include "library/SourceRunner.h"
#include "itch/ItchImporter.h"
#include "itch/ItchInstaller.h"
#include "launchers/Launchers.h"
#include "lutris/LutrisImporter.h"
#include "metadata/MetadataFetcher.h"
#include "proc/ProcessIndex.h"
#include "proc/ProcessSupervisor.h"
#include "proc/Session.h"
#include "runner/Downloader.h"
#include "runner/Exec.h"
#include "runner/GameMode.h"
#include "runner/ProtonRunner.h"
#include "runner/RunnerRegistry.h"
#include "runner/Winetricks.h"
#include "steam/SteamScanner.h"

namespace mira::api {
namespace {
using nlohmann::json;
using httplib::Request;
using httplib::Response;

json ErrorBody(std::string_view code, std::string_view message) {
  return {{"error", {{"code", code}, {"message", message}}}};
}

void SendError(Response& res, int status, std::string_view code, std::string_view message) {
  res.status = status;
  res.set_content(ErrorBody(code, message).dump(), "application/json");
}


// Same, keeping the error's hint and fix for the client to offer.
void SendError(Response& res, int status, const Error& error) {
  json body = ErrorBody(error.code, error.message);
  AddHintAndFix(body["error"], error);
  res.status = status;
  res.set_content(body.dump(), "application/json");
}

// body[key] as strings: empty if absent, nullopt if not an array of strings.
std::optional<std::vector<std::string>> StringList(const json& body, const char* key) {
  if (!body.contains(key)) return std::vector<std::string>{};
  if (!body[key].is_array()) return std::nullopt;
  std::vector<std::string> out;
  for (const json& item : body[key]) {
    if (!item.is_string()) return std::nullopt;
    out.push_back(item.get<std::string>());
  }
  return out;
}

Error GameRunningError(const std::string& id) {
  return Error{"game_running", std::format("\"{}\" is running", id), "Stop the game first.", {}};
}

// One game's failure inside a batch reply: {id, error: {code, message, hint?, fix?}}.
json BatchFailure(const std::string& id, const Error& error) {
  json body = ErrorBody(error.code, error.message);
  AddHintAndFix(body["error"], error);
  body["id"] = id;
  return body;
}

// A GameStore failure: an unknown id is 404, a failed save is the daemon's fault.
void SendStoreError(Response& res, const Error& error) {
  SendError(res, error.code == "game_not_found" ? 404 : 500, error);
}

void SendJson(Response& res, json body, int status = 200) {
  res.status = status;
  res.set_content(body.dump(), "application/json");
}

void SendResult(Response& res, const Result<void>& result) {
  if (result) {
    SendJson(res, json::object());
  } else {
    SendError(res, 400, result.error());
  }
}

model::Game ParseGamePatch(const model::Game& base, const json& patch) {
  model::Game game = base;
  if (patch.contains("name") && patch["name"].is_string()) game.name = patch["name"];
  if (patch.contains("exe_path") && patch["exe_path"].is_string()) game.exe_path = patch["exe_path"];
  if (patch.contains("args") && patch["args"].is_string()) game.args = patch["args"];
  if (patch.contains("working_dir") && patch["working_dir"].is_string()) {
    game.working_dir = patch["working_dir"];
  }
  if (patch.contains("runner_ref") && patch["runner_ref"].is_string()) {
    game.runner_ref = patch["runner_ref"];
  }
  if (patch.contains("data_dir") && patch["data_dir"].is_string()) {
    game.data_dir = patch["data_dir"];
  }
  if (patch.contains("runner_config") && patch["runner_config"].is_object()) {
    game.runner_config.merge_patch(patch["runner_config"]);
  }
  // Replaced, not merged; {"tags": []} clears them.
  if (patch.contains("tags") && patch["tags"].is_array()) {
    game.tags.clear();
    for (const auto& tag : patch["tags"]) {
      if (tag.is_string()) game.tags.push_back(tag.get<std::string>());
    }
  }
  // "env": null clears every entry; {"K": null} removes just K.
  if (patch.contains("env") && patch["env"].is_null()) {
    game.env.clear();
  } else if (patch.contains("env") && patch["env"].is_object()) {
    for (const auto& [key, value] : patch["env"].items()) {
      if (value.is_null()) {
        game.env.erase(key);
      } else if (value.is_string()) {
        game.env[key] = value.get<std::string>();
      }
    }
  }
  // A correction counts as the human having looked; a patch that changed nothing doesn't.
  if (model::ToJson(game) != model::ToJson(base)) game.reviewed = true;
  if (patch.contains("reviewed") && patch["reviewed"].is_boolean()) game.reviewed = patch["reviewed"];
  return game;
}

// `needs_check`: Mira picked the executable itself, wasn't sure, and nobody has confirmed it yet.
void AddNeedsCheck(json& game, double threshold) {
  const json candidates = game.value("candidates", json::array());
  game["needs_check"] = candidates.is_array() && !candidates.empty() && !game.value("reviewed", false) &&
                        game.value("confidence", 1.0) < threshold;
}

// The first wrong-typed field of a game patch, named, so a bad body is a 400 rather than silently ignored.
std::optional<std::string> GamePatchProblem(const json& patch) {
  if (!patch.is_object()) return "expected a JSON object";
  for (const char* key : {"name", "exe_path", "args", "working_dir", "runner_ref", "data_dir"}) {
    if (patch.contains(key) && !patch[key].is_string()) return std::format("\"{}\" must be a string", key);
  }
  if (patch.contains("tags") && !patch["tags"].is_array()) return "\"tags\" must be an array";
  if (patch.contains("runner_config") && !patch["runner_config"].is_object()) return "\"runner_config\" must be an object";
  if (patch.contains("env") && !patch["env"].is_object() && !patch["env"].is_null()) return "\"env\" must be an object or null";
  if (patch.contains("reviewed") && !patch["reviewed"].is_boolean()) return "\"reviewed\" must be true or false";
  return std::nullopt;
}

// Applies a flat {"dotted.key": value} overrides patch; null removes an override.
void ApplyOverridesPatch(model::Game& game, const json& patch) {
  for (const auto& [key, value] : patch.items()) {
    if (value.is_null()) {
      game.overrides.erase(key);
    } else {
      game.overrides[key] = value;
    }
  }
}

// Checked first so a bad key rejects the whole patch, like Config::Patch.
std::optional<std::string> ValidateOverridesPatch(const json& patch) {
  for (const auto& [key, value] : patch.items()) {
    if (value.is_null()) continue;  // removal; nothing to validate
    if (!config::Resolver::IsOverridable(key)) {
      return std::format("\"{}\" cannot be overridden per game", key);
    }
    if (auto problem = config::Schema::Instance().Validate(key, value)) {
      return std::format("{}: {}", key, *problem);
    }
  }
  return std::nullopt;
}

// The first wrapper ends up outermost. Entries are split on spaces, with no quoting.
void ApplyCommandWrappers(Command& command, const std::vector<std::string>& wrappers) {
  for (auto it = wrappers.rbegin(); it != wrappers.rend(); ++it) {
    if (it->empty()) continue;
    const std::vector<std::string> tokens = strings::Split(*it, ' ');
    command.argv.insert(command.argv.begin(), tokens.begin(), tokens.end());
  }
}

// Fails with the missing wrapper's name instead of an exit code 127 from inside it.
Result<void> CheckCommandWrappers(const std::vector<std::string>& wrappers) {
  for (const std::string& entry : wrappers) {
    if (entry.empty()) continue;
    const std::vector<std::string> tokens = strings::Split(entry, ' ');
    if (tokens.empty()) continue;
    if (!runner::FindOnPath(tokens[0])) {
      return Err("wrapper_not_found", std::format("the command wrapper \"{}\" isn't installed", tokens[0]),
                 "Install it, or remove it from Command Wrappers.", Fix::Setting("command_wrappers"));
    }
  }
  return {};
}

// Applied under the runner's env, so the game's own env still wins.
void ApplyLaunchEnv(Command& command, const std::vector<std::string>& entries) {
  for (const std::string& entry : entries) {
    const auto eq = entry.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = entry.substr(0, eq);
    if (!command.env.contains(key)) command.env[key] = entry.substr(eq + 1);
  }
}

// Used by the Steam handoff and the no-mira-run fallback; otherwise mira-run runs the script.
Result<void> RunPreScriptInline(const std::string& pre_script) {
  if (pre_script.empty()) return {};
  Command script;
  script.argv = {"sh", "-c", pre_script};
  const Result<runner::ExecResult> ran = runner::RunAndWait(script);
  if (!ran || ran->exit_code != 0) {
    return Err("pre_launch_failed",
               !ran ? ran.error().message
                    : std::format("the pre-launch script exited {}: {}", ran->exit_code, ran->output),
               "Fix or clear the pre-launch script.", Fix::Setting("launch.pre_script"));
  }
  return {};
}

// Empty on failure; ResolveSiblingBinary then falls back to PATH.
std::filesystem::path OwnBinaryDir() {
  std::error_code ec;
  const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path() : exe.parent_path();
}

struct WrapperStatus {
  bool ok = false;
  bool read_timed_out = false;  // mirad's own read deadline, distinct from mira-run's launch.pre_timeout_s
  std::string code;             // "ok" / "pre_failed" / "pre_timeout"
  std::string detail;           // session path (ok) or the pre script's captured output (pre_failed)
};

// Blocks until mira-run writes its status and closes the pipe, or `timeout_s` passes.
WrapperStatus ReadWrapperStatus(int fd, int timeout_s) {
  WrapperStatus result;
  std::string buffer;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
  char chunk[4096];
  while (std::chrono::steady_clock::now() < deadline && buffer.size() < 65536) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    const int rc = ::poll(&pfd, 1, static_cast<int>(std::max<std::chrono::milliseconds::rep>(0, remaining.count())));
    if (rc <= 0) break;  // timed out, or poll itself failed
    const ssize_t n = ::read(fd, chunk, sizeof(chunk));
    if (n <= 0) break;  // EOF: mira-run closed its end after writing everything
    buffer.append(chunk, static_cast<std::size_t>(n));
  }
  const auto newline = buffer.find('\n');
  if (newline == std::string::npos) {
    result.read_timed_out = true;
    return result;
  }
  result.code = buffer.substr(0, newline);
  result.detail = buffer.substr(newline + 1);
  // Strip mira-run's trailing newline from the session path.
  while (!result.detail.empty() && (result.detail.back() == '\n' || result.detail.back() == '\r')) {
    result.detail.pop_back();
  }
  result.ok = (result.code == "ok");
  return result;
}

json CollectionJson(const itch::ItchCollection& collection) {
  return {{"id", collection.id},
          {"title", collection.title},
          {"games_count", collection.games_count},
          {"own", collection.own},
          {"url", std::format("https://itch.io/c/{}", collection.id)}};
}

// A preview is saved without an extension, so its type comes from its bytes.
std::string SniffImageType(std::string_view bytes) {
  if (bytes.starts_with("\x89PNG")) return "image/png";
  if (bytes.starts_with("GIF8")) return "image/gif";
  if (bytes.size() >= 12 && bytes.starts_with("RIFF") && bytes.substr(8, 4) == "WEBP") return "image/webp";
  return "image/jpeg";
}

// Serves one cached art slot for `id`, or 404s.
void SendCachedArtwork(const config::Config& config, const std::string& id, const std::string& type,
                       Response& res) {
  const std::string key = type == "cover" ? "artwork" : type;
  std::ifstream meta_in(metadata::MetadataFile(config, id));
  if (!meta_in) return SendError(res, 404, "artwork_not_found", "no artwork cached for this game yet");
  const json info = json::parse(meta_in, nullptr, false);
  if (info.is_discarded() || !info.contains(key)) {
    return SendError(res, 404, "artwork_not_found", "no artwork cached for this game yet");
  }
  const std::filesystem::path file = metadata::ArtworkDir(config, id) / info[key].value("file", std::string());
  std::ifstream in(file, std::ios::binary);
  if (!in) return SendError(res, 404, "artwork_not_found", "cached artwork file is missing");
  std::ostringstream buffer;
  buffer << in.rdbuf();
  res.set_content(buffer.str(), info[key].value("content_type", "image/jpeg"));
}

// A store ref ends up in a path ("<source>-<ref>" art) and on a store
// tool's command line, where a leading '-' would read as an option.
bool IsSafeRef(const std::string& ref) {
  return !ref.empty() && !ref.starts_with('-') && ref.find('/') == std::string::npos &&
         ref.find('\0') == std::string::npos;
}

void SyncDesktopEntries(config::Config& config, store::GameStore& games) {
  if (auto synced = desktop::DesktopEntries(config).Sync(games.All()); !synced) {
    log::Warn("could not update application menu entries: {}", synced.error().message);
  }
}

// Deletes `target` only if it resolves (symlinks included) inside one of `roots`.
Result<void> DeleteUnderRoot(const std::string& target, const std::vector<std::filesystem::path>& roots) {
  return library::DeleteInside(target, roots);
}

// A build's own directory. Wine's path is <dir>/bin/wine.
std::filesystem::path BuildDir(const model::RunnerBuild& build) {
  const std::filesystem::path path(build.path);
  return build.kind == "wine" ? path.parent_path().parent_path() : path;
}

// The folders Mira installs builds of `kind` into, and may delete them from.
std::vector<std::filesystem::path> RunnerRoots(const config::Config& config, const std::string& kind) {
  return config.GetPathArray(kind == "wine" ? "wine_search_paths" : "runner_search_paths");
}

// `source`, or the kind's preferred source when empty.
Result<runner::RunnerFamily> FamilyFor(const config::Config& config, const std::string& kind,
                                       const std::string& source) {
  if (source.empty()) {
    const auto families = runner::Families(config, kind);
    if (families.empty()) return Err("unknown_runner_kind", std::format("nothing to download for \"{}\"", kind));
    return families.front();
  }
  auto family = runner::FindFamily(config, source);
  if (!family || family->kind != kind) {
    return Err("unknown_runner_source", std::format("no {} source \"{}\"", kind, source));
  }
  return *family;
}

std::vector<model::RunnerBuild> BuildsOfKind(const runner::RunnerRegistry& registry, const std::string& kind) {
  std::vector<model::RunnerBuild> out = registry.DiscoverAll();
  std::erase_if(out, [&](const model::RunnerBuild& build) { return build.kind != kind; });
  return out;
}

bool HasInstalled(const std::vector<model::RunnerBuild>& builds, const runner::ReleaseAsset& release) {
  return std::ranges::any_of(builds, [&](const model::RunnerBuild& build) {
    return runner::IsInstalledAs(build.kind, build.name, BuildDir(build).filename().string(), release);
  });
}

struct RunnerUpdate {
  model::RunnerBuild build;
  runner::RunnerFamily family;
  runner::ReleaseAsset latest;
};

// Removable builds whose source's newest release isn't installed yet.
std::vector<RunnerUpdate> FindRunnerUpdates(const config::Config& config, const runner::RunnerRegistry& registry) {
  std::vector<RunnerUpdate> out;
  for (const std::string kind : {"proton", "wine"}) {
    const std::vector<model::RunnerBuild> builds = BuildsOfKind(registry, kind);
    for (const model::RunnerBuild& build : builds) {
      const std::filesystem::path dir = BuildDir(build);
      if (!paths::IsWithin(dir, RunnerRoots(config, kind))) continue;
      auto family = runner::FamilyOfBuild(config, kind, build.name, dir.filename().string());
      if (!family) continue;
      auto releases = runner::ListFamilyReleases(*family);
      if (!releases || releases->empty() || HasInstalled(builds, releases->front())) continue;
      out.push_back({build, std::move(*family), releases->front()});
    }
  }
  return out;
}

}  // namespace

Server::Server(config::Config& config, store::GameStore& games, EventBus& events)
    : config_(config),
      games_(games),
      events_(events),
      http_(std::make_unique<httplib::Server>()),
      supervisor_(games, events, config.GetInt("launch.stop_timeout_s")) {
  // Importers and the scanner publish bare records; this gives every game
  // event the same `running` and `art` as GET /v1/games.
  events_.SetGameRecordHook([this](json& game) {
    const std::string id = game.value("id", "");
    game["running"] = supervisor_.IsRunning(id);
    game["art"] = art_index_.For(id);
    AddNeedsCheck(game, config_.GetDouble("detect.low_confidence_threshold"));
  });
  events_.SetArtHook([this](const std::string& id) { return art_index_.For(id); });
  supervisor_.SetExitHook([this](const std::string& id) { CheckForInstall(id); });
}

void Server::CheckForInstall(const std::string& game_id) {
  std::set<std::filesystem::path> before;
  {
    const std::lock_guard lock(install_watch_mutex_);
    auto watched = install_watch_.extract(game_id);
    if (watched.empty()) return;
    before = std::move(watched.mapped());
  }
  const auto game = games_.Find(game_id);
  if (!game) return;
  const auto installed = library::NewInstall(config_, game->data_dir, before);
  if (!installed) return;
  log::Info("{} installed {} when run; asking whether to use it", game_id, installed->dir.string());
  events_.Publish("game.install_detected",
                  {{"id", game_id}, {"install_path", installed->dir.string()}, {"exe_path", installed->exe_path}});
}

json Server::Record(const model::Game& game) {
  json body = model::ToJson(game);
  // So a client can resync after a reconnect.
  body["running"] = supervisor_.IsRunning(game.id);
  body["art"] = art_index_.For(game.id);
  AddNeedsCheck(body, config_.GetDouble("detect.low_confidence_threshold"));
  return body;
}

Server::~Server() {
  events_.SetGameRecordHook(nullptr);
  events_.SetArtHook(nullptr);
  stopping_.store(true, std::memory_order_relaxed);
  if (external_watch_.joinable()) external_watch_.join();
}

// Picks up games started outside Mira (the Steam client, a running launcher) so
// they show as playing and count playtime.
void Server::WatchExternalGames() {
  constexpr auto kTick = std::chrono::milliseconds(500);
  constexpr int kTicksPerScan = 6;
  proc::ProcessIndex index;
  for (int tick = 0; !stopping_.load(std::memory_order_relaxed); ++tick) {
    std::this_thread::sleep_for(kTick);
    if (tick % kTicksPerScan != 0) continue;

    struct Candidate {
      model::Game game;
      std::string appid;    // Steam
      std::string win_dir;  // launcher
    };
    std::vector<Candidate> candidates;
    for (const model::Game& game : games_.All()) {
      if (supervisor_.IsRunning(game.id)) continue;
      if (game.runner_ref.starts_with("steam:")) {
        if (config::Resolver(config_, game.overrides).GetBool("steam.track_process")) {
          candidates.push_back({game, game.runner_ref.substr(6), ""});
        }
      } else if (launchers::Find(game.source) && !game.data_dir.empty()) {
        candidates.push_back({game, "", launchers::WindowsDir(game)});
      }
    }
    if (candidates.empty()) continue;  // nothing to watch: don't touch /proc

    index.Refresh();
    std::set<std::string> steam_running;
    for (const auto& [pid, info] : index.Processes()) {
      if (!info.steam_launch.empty()) steam_running.insert(info.steam_launch);
    }
    for (const Candidate& candidate : candidates) {
      const std::string post_script =
          config::Resolver(config_, candidate.game.overrides).GetString("launch.post_script");
      if (!candidate.appid.empty()) {
        if (!steam_running.contains(candidate.appid)) continue;
        if (supervisor_.TrackSteamLaunch(candidate.game, candidate.appid, post_script)) {
          events_.Publish("game.launched", {{"id", candidate.game.id}, {"via", "steam"}, {"tracked", true}});
        }
        continue;
      }
      const bool running = std::ranges::any_of(index.Processes(), [&](const auto& item) {
        return proc::InPrefix(item.second.prefix, candidate.game.data_dir) &&
               item.second.argv0.starts_with(candidate.win_dir + "/");
      });
      if (running && supervisor_.TrackLauncherLaunch(candidate.game, candidate.win_dir, 10, post_script)) {
        events_.Publish("game.launched", {{"id", candidate.game.id}, {"via", "launcher"}, {"tracked", true}});
      }
    }
  }
}

void Server::QueueMetadata(const std::vector<model::Game>& games) {
  for (const model::Game& game : games) metadata_fetches_.Enqueue(config_, events_, game);
}

void Server::ReconcileSessions() {
  supervisor_.Reconcile(games_.Dir() / "sessions");
  // A client that stayed open across a restart may still show games from
  // the old daemon as running.
  for (const model::Game& game : games_.All()) {
    if (supervisor_.IsRunning(game.id)) continue;
    json event = Record(game);
    event["state"] = "idle";
    events_.Publish("game.state", std::move(event));
  }
}

Result<void> Server::Serve(const std::filesystem::path& socket_path) {
  std::error_code ec;
  std::filesystem::create_directories(socket_path.parent_path(), ec);
  std::filesystem::remove(socket_path, ec);  // clear a stale socket from an unclean shutdown

  RegisterRoutes();

  // httplib's default is one thread per core with a floor of 8, and every
  // event stream and every long request (a scan, a move) holds one for its
  // whole length. Idle threads cost almost nothing.
  http_->new_task_queue = [] { return new httplib::ThreadPool(32); };
  http_->set_address_family(AF_UNIX);
  if (!http_->bind_to_port(socket_path.string(), 80)) {
    return Err("socket_bind_failed", std::format("cannot bind {}", socket_path.string()));
  }
  std::filesystem::permissions(socket_path, std::filesystem::perms::owner_read |
                                                std::filesystem::perms::owner_write,
                               ec);

  log::Info("listening on {}", socket_path.string());
  if (stopping_) return {};  // Stop() came before listening, where it would have done nothing
  external_watch_ = std::thread(&Server::WatchExternalGames, this);
  if (!http_->listen_after_bind()) {
    return Err("socket_listen_failed", "httplib server exited unexpectedly");
  }
  return {};
}

void Server::Stop() {
  stopping_.store(true, std::memory_order_relaxed);
  http_->stop();
}

void Server::RegisterRoutes() {
  // A body field of the wrong type throws out of a route; answer in the JSON envelope instead of a bare 500.
  http_->set_exception_handler([](const Request&, Response& res, std::exception_ptr error) {
    try {
      std::rethrow_exception(error);
    } catch (const json::exception& e) {
      SendError(res, 400, "invalid_body", e.what());
    } catch (const std::exception& e) {
      SendError(res, 500, "internal_error", e.what());
    }
  });
  // Unknown paths and wrong methods arrive with no body.
  http_->set_error_handler([](const Request&, Response& res) {
    if (!res.body.empty()) return;
    SendError(res, res.status, res.status == 405 ? "method_not_allowed" : "not_found",
              res.status == 405 ? "that method isn't supported here" : "no such endpoint");
  });
  // A store's import as a job; `Importer` is its XxxImporter.
  const auto import_job = [this]<typename Importer>(const Request& req, Response& res, std::type_identity<Importer>,
                                                    const std::string& source, const std::string& label) {
    StartJob(req, res, "import", source, label, [this](JobRegistry::Progress&) -> Result<json> {
      Importer importer(config_, games_, events_);
      auto summary = importer.Import();
      if (!summary) return std::unexpected(summary.error());
      SyncDesktopEntries(config_, games_);
      for (const model::Game& game : summary->added_games) metadata_fetches_.Enqueue(config_, events_, game);
      return json{{"added", summary->added}, {"updated", summary->updated}};
    });
  };

  http_->Get("/v1/jobs/([A-Za-z0-9_-]+)", [this](const Request& req, Response& res) {
    const auto job = jobs_.Find(req.matches[1].str());
    if (!job) return SendError(res, 404, "job_not_found", "no such job, or it finished too long ago");
    SendJson(res, *job);
  });

  http_->Get("/v1/health", [](const Request&, Response& res) {
    SendJson(res, {{"status", "ok"}, {"api", kApiVersion}});
  });

  http_->Get("/v1/gamemode/status", [](const Request&, Response& res) {
    SendJson(res, {{"installed", gamemode::IsInstalled()}, {"daemon_running", gamemode::IsDaemonRunning()}});
  });

  // --- settings -----------------------------------------------------------

  http_->Get("/v1/config", [this](const Request&, Response& res) {
    json body = config_.Document();
    body["frontend"] = config_.FrontendSettings();
    SendJson(res, std::move(body));
  });

  http_->Get("/v1/config/schema", [](const Request&, Response& res) {
    json entries = json::array();
    for (const config::Entry& entry : config::Schema::Instance().Entries()) {
      entries.push_back({
          {"key", entry.key},
          {"label", entry.label},
          {"type", config::ToString(entry.type)},
          {"default", entry.default_value},
          {"scope", config::ToString(entry.scope)},
          {"doc", entry.doc},
          {"category", entry.category},
          {"group", entry.group},
          {"group_label", entry.group_label},
      });
      // Present only when there's a shape to describe.
      if (!entry.constraint.one_of.empty()) entries.back()["one_of"] = entry.constraint.one_of;
      if (entry.constraint.minimum) entries.back()["minimum"] = *entry.constraint.minimum;
      if (entry.constraint.maximum) entries.back()["maximum"] = *entry.constraint.maximum;
      if (!entry.game_doc.empty()) entries.back()["game_doc"] = entry.game_doc;
      if (entry.is_secret) entries.back()["is_secret"] = true;
      if (entry.is_runner_ref) entries.back()["is_runner_ref"] = true;
      if (!entry.link.empty()) entries.back()["link"] = entry.link;
      if (!entry.keywords.empty()) entries.back()["keywords"] = entry.keywords;
      if (entry.group_collapsed) entries.back()["group_collapsed"] = true;
      if (entry.group_resettable) entries.back()["group_resettable"] = true;
      if (!entry.source.empty()) entries.back()["source"] = entry.source;
      if (entry.path != config::PathKind::None) {
        entries.back()["path"] = entry.path == config::PathKind::Folder ? "folder" : "file";
      }
    }
    SendJson(res, std::move(entries));
  });

  http_->Patch("/v1/config", [this](const Request& req, Response& res) {
    json patch = json::parse(req.body, nullptr, false);
    if (patch.is_discarded()) return SendError(res, 400, "invalid_json", "body is not valid JSON");
    Result<void> result = config_.Patch(patch);
    if (result) SyncDesktopEntries(config_, games_);
    if (result && patch.is_object() && patch.contains("library_roots") && on_roots_changed_) on_roots_changed_();
    SendResult(res, result);
  });

  http_->Post("/v1/config/reset", [this](const Request& req, Response& res) {
    Result<void> result;
    bool roots_reset = true;
    if (auto it = req.params.find("key"); it != req.params.end()) {
      result = config_.Reset(it->second);
      roots_reset = it->second == "library_roots";
    } else {
      config_.ResetAll();
      result = config_.Save();
    }
    if (result) SyncDesktopEntries(config_, games_);
    if (result && roots_reset && on_roots_changed_) on_roots_changed_();
    SendResult(res, result);
  });

  // --- games ----------------------------------------------------------------

  // Games tagged "hidden" are left out unless a tag is asked for, or include_hidden=true.
  http_->Get("/v1/games", [this](const Request& req, Response& res) {
    std::vector<model::Game> all = games_.All();
    json out = json::array();
    const auto status_filter = req.params.find("status");
    const auto tag_filter = req.params.find("tag");
    const bool include_hidden = req.get_param_value("include_hidden") == "true";
    for (const model::Game& game : all) {
      if (status_filter != req.params.end() &&
          status_filter->second != model::ToString(game.status)) {
        continue;
      }
      if (tag_filter != req.params.end()) {
        if (!std::ranges::contains(game.tags, tag_filter->second)) continue;
      } else if (!include_hidden && std::ranges::contains(game.tags, std::string("hidden"))) {
        continue;
      }
      out.push_back(Record(game));
    }
    SendJson(res, std::move(out));
  });

  http_->Get(R"(/v1/games/([^/]+))", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    json body = Record(*game);
    // What an empty runner_ref runs with, so an editor can show that runner's options.
    model::Game unpinned = *game;
    unpinned.runner_ref.clear();
    body["default_runner"] = runner::RunnerRegistry(config_).ResolveRef(unpinned);
    SendJson(res, body);
  });

  // The tail of mira-run's log for this game. No log yet is an empty list.
  http_->Get(R"(/v1/games/([^/]+)/log)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    int requested_lines = 200;
    if (auto it = req.params.find("lines"); it != req.params.end()) {
      requested_lines = std::max(1, std::atoi(it->second.c_str()));
    }

    const std::filesystem::path log_file = games_.Dir() / "logs" / std::format("{}.log", game->id);
    std::ifstream in(log_file, std::ios::binary);
    if (!in) return SendJson(res, {{"lines", json::array()}});

    // Only the end of the file is read; logs can be up to launch.log_max_mb.
    constexpr std::streamoff kMaxTailBytes = 4 * 1024 * 1024;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(size > kMaxTailBytes ? size - kMaxTailBytes : 0);
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    std::vector<std::string> all_lines = strings::Split(content, '\n');
    if (!all_lines.empty() && all_lines.back().empty()) all_lines.pop_back();  // trailing newline
    const std::size_t take = std::min(all_lines.size(), static_cast<std::size_t>(requested_lines));
    json out = json::array();
    for (std::size_t i = all_lines.size() - take; i < all_lines.size(); ++i) out.push_back(all_lines[i]);
    SendJson(res, {{"lines", out}});
  });

  http_->Patch(R"(/v1/games/([^/]+))", [this](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    json patch = json::parse(req.body, nullptr, false);
    if (patch.is_discarded()) return SendError(res, 400, "invalid_json", "body is not valid JSON");
    if (const auto problem = GamePatchProblem(patch)) return SendError(res, 400, "invalid_body", *problem);

    auto result = games_.Update(id, [&](model::Game& game) { game = ParseGamePatch(game, patch); });
    if (!result) return SendStoreError(res, result.error());
    SyncDesktopEntries(config_, games_);
    events_.Publish("game.updated", Record(*result));
    SendJson(res, Record(*result));
  });

  // One save, one menu sync and one event for any number of games, so a
  // multi-select doesn't cost a request (and a full rewrite) per game.
  http_->Patch("/v1/games", [this](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    const auto ids = body.is_object() ? StringList(body, "ids") : std::nullopt;
    const auto add_tags = body.is_object() ? StringList(body, "add_tags") : std::nullopt;
    const auto remove_tags = body.is_object() ? StringList(body, "remove_tags") : std::nullopt;
    const json config = body.is_object() ? body.value("config", json::object()) : json();
    if (!ids || !add_tags || !remove_tags || !config.is_object()) {
      return SendError(res, 400, "invalid_body",
                       R"(expected {"ids": [...], "add_tags"?: [...], "remove_tags"?: [...], "config"?: {...}})");
    }
    if (auto problem = ValidateOverridesPatch(config)) return SendError(res, 400, "invalid_setting", *problem);

    auto updated = games_.UpdateMany(*ids, [&](model::Game& game) {
      const std::vector<std::string> old_tags = game.tags;
      const json old_overrides = game.overrides;
      std::erase_if(game.tags, [&](const std::string& tag) { return std::ranges::contains(*remove_tags, tag); });
      for (const std::string& tag : *add_tags) {
        if (!std::ranges::contains(game.tags, tag)) game.tags.push_back(tag);
      }
      ApplyOverridesPatch(game, config);
      return game.tags != old_tags || game.overrides != old_overrides;
    });
    if (!updated) return SendStoreError(res, updated.error());

    json games = json::array();
    for (const model::Game& game : *updated) games.push_back(Record(game));
    if (!updated->empty()) {
      // Tags never change a menu entry; only an override can.
      if (!config.empty()) SyncDesktopEntries(config_, games_);
      events_.Publish("games.updated", {{"games", games}});
    }
    SendJson(res, {{"games", std::move(games)}});
  });

  http_->Get(R"(/v1/games/([^/]+)/config)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    config::Resolver resolver(config_, game->overrides);
    SendJson(res, resolver.EffectiveDocument());
  });

  http_->Patch(R"(/v1/games/([^/]+)/config)", [this](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    json patch = json::parse(req.body, nullptr, false);
    if (patch.is_discarded() || !patch.is_object()) {
      return SendError(res, 400, "invalid_json", "expected a flat {\"dotted.key\": value} object");
    }
    if (auto problem = ValidateOverridesPatch(patch)) {
      return SendError(res, 400, "invalid_setting", *problem);
    }
    auto result =
        games_.Update(id, [&](model::Game& game) { ApplyOverridesPatch(game, patch); });
    if (!result) return SendStoreError(res, result.error());
    // An override can turn desktop_entries.enabled off for this game.
    SyncDesktopEntries(config_, games_);
    SendJson(res, Record(*result));
  });

  http_->Delete(R"(/v1/games/([^/]+))", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    const bool purge = req.has_param("purge") && req.get_param_value("purge") == "true";
    const auto flag = [&](const char* name) {
      return purge || (req.has_param(name) && req.get_param_value(name) == "true");
    };
    const auto folders_lock = games_.LockFolders();
    if (auto deleted = DeleteGameData(*game, flag("delete_files"), flag("delete_prefix"), flag("delete_metadata"));
        !deleted) {
      return SendError(res, deleted.error().code == "game_running" ? 409 : 400, deleted.error());
    }

    auto result = games_.Remove(req.matches[1]);
    if (!result) return SendStoreError(res, result.error());
    SyncDesktopEntries(config_, games_);
    events_.Publish("game.removed", {{"id", req.matches[1].str()}});
    SendJson(res, json::object());
  });

  // DELETE /v1/games/{id} for many games, with one save, menu sync and event.
  // A game whose files can't be deleted stays in the library.
  http_->Post("/v1/games/delete", [this](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    const auto ids = body.is_object() ? StringList(body, "ids") : std::nullopt;
    if (!ids) {
      return SendError(res, 400, "invalid_body",
                       R"(expected {"ids": [...], "delete_files"?, "delete_prefix"?, "delete_metadata"?})");
    }
    const bool purge = body.value("purge", false);
    const bool files = purge || body.value("delete_files", false);
    const bool prefix = purge || body.value("delete_prefix", false);
    const bool metadata = purge || body.value("delete_metadata", false);

    StartJob(req, res, "delete", "", "Removing games", [this, ids = *ids, files, prefix, metadata](
                                                            JobRegistry::Progress& progress) -> Result<json> {
      std::vector<std::string> deletable;
      json failed = json::array();
      auto folders_lock = games_.LockFolders();
      int done = 0;
      for (const std::string& id : ids) {
        progress.Report(done++, static_cast<int>(ids.size()));
        const auto game = games_.Find(id);
        if (!game) continue;
        if (auto deleted = DeleteGameData(*game, files, prefix, metadata); !deleted) {
          failed.push_back(BatchFailure(id, deleted.error()));
          continue;
        }
        deletable.push_back(id);
      }
      auto removed = games_.RemoveMany(deletable);
      folders_lock.unlock();
      if (!removed) return std::unexpected(removed.error());
      if (!removed->empty()) {
        SyncDesktopEntries(config_, games_);
        events_.Publish("games.removed", {{"ids", *removed}});
      }
      return json{{"removed", *removed}, {"failed", std::move(failed)}};
    });
  });

  // --- library ------------------------------------------------------------

  http_->Post("/v1/library/scan", [this](const Request& req, Response& res) {
    StartJob(req, res, "scan", "", "Scanning your library", [this](JobRegistry::Progress&) -> Result<json> {
      library::Scanner scanner(config_, games_, events_);
      const library::ScanSummary summary = scanner.ScanAll();
      for (const model::Game& game : summary.added_games) metadata_fetches_.Enqueue(config_, events_, game);
      return json{{"added", summary.added}, {"missing", summary.missing}, {"restored", summary.restored}};
    });
  });

  // One game at a time, since a move can copy a whole game. Each moved game
  // publishes its own game.updated.
  http_->Post("/v1/library/relocate", [this](const Request& req, Response& res) {
    const json body = req.body.empty() ? json::object() : json::parse(req.body, nullptr, false);
    const auto ids = body.is_object() ? StringList(body, "ids") : std::nullopt;
    if (!ids) return SendError(res, 400, "invalid_body", R"(expected no body, or {"ids": [...]})");

    std::vector<model::Game> games = games_.All();
    if (body.contains("ids")) {
      std::erase_if(games, [&](const model::Game& game) { return !std::ranges::contains(*ids, game.id); });
    }
    StartJob(req, res, "relocate", "", "Moving games into Mira's folders",
             [this, games = std::move(games)](JobRegistry::Progress& progress) -> Result<json> {
               int moved = 0;
               int done = 0;
               json errors = json::array();
               for (const model::Game& game : games) {
                 progress.Report(done++, static_cast<int>(games.size()), game.name);
                 if (supervisor_.IsRunning(game.id)) {
                   errors.push_back(BatchFailure(game.id, GameRunningError(game.id)));
                   continue;
                 }
                 // Per game, so scans can run between moves.
                 auto folders_lock = games_.LockFolders();
                 auto relocated = library::Relocate(config_, game);
                 if (!relocated) {
                   log::Warn("relocate failed for {}: {}", game.id, relocated.error().message);
                   errors.push_back(BatchFailure(game.id, relocated.error()));
                   continue;
                 }
                 if (relocated->install_path == game.install_path && relocated->data_dir == game.data_dir) continue;
                 auto saved = games_.Update(game.id, [&](model::Game& g) {
                   g.install_path = relocated->install_path;
                   g.data_dir = relocated->data_dir;
                   g.updated_at = model::NowSeconds();
                 });
                 if (!saved) {
                   errors.push_back(BatchFailure(game.id, saved.error()));
                   continue;
                 }
                 events_.Publish("game.updated", Record(*saved));
                 ++moved;
               }
               SyncDesktopEntries(config_, games_);
               const int failed = static_cast<int>(errors.size());
               return json{{"moved", moved}, {"failed", failed}, {"errors", std::move(errors)}};
             });
  });

  // --- steam ------------------------------------------------------------

  http_->Post("/v1/steam/scan", [this](const Request& req, Response& res) {
    StartJob(req, res, "import", "steam", "Importing from Steam", [this](JobRegistry::Progress&) -> Result<json> {
      steam::SteamScanner scanner(config_, games_, events_);
      auto summary = scanner.Scan();
      if (!summary) return std::unexpected(summary.error());
      SyncDesktopEntries(config_, games_);
      for (const model::Game& game : summary->added_games) metadata_fetches_.Enqueue(config_, events_, game);
      return json{{"added", summary->added}, {"updated", summary->updated}};
    });
  });

  // --- lutris -----------------------------------------------------------

  http_->Post("/v1/lutris/import", [this](const Request& req, Response& res) {
    StartJob(req, res, "import", "lutris", "Importing from Lutris", [this](JobRegistry::Progress&) -> Result<json> {
      lutris::LutrisImporter importer(config_, games_, events_);
      auto summary = importer.Import();
      if (!summary) return std::unexpected(summary.error());
      SyncDesktopEntries(config_, games_);
      for (const model::Game& game : summary->added_games) metadata_fetches_.Enqueue(config_, events_, game);
      return json{{"added", summary->added},
                  {"updated", summary->updated},
                  {"other_runner", summary->other_runner},
                  {"incomplete", summary->incomplete}};
    });
  });

  // --- store CLI setup ----------------------------------------------------

  // POST <route> downloads the newest release of a store's CLI in the
  // background. Re-running it fetches the latest release, which is also how
  // updates work. Progress arrives as <event_prefix>.started/.finished/.failed.
  using BinaryInstaller = Result<void> (*)(const config::Config&, const runner::ReleaseAsset&);
  const auto register_cli_setup = [this](const char* route, const std::string& kind, const std::string& tool,
                                         const std::string& event_prefix, BinaryInstaller install) {
    http_->Post(route, [this, kind, tool, event_prefix, install](const Request&, Response& res) {
      auto releases = runner::ListReleases(config_, kind);
      if (!releases) return SendError(res, 502, releases.error());
      if (releases->empty()) {
        return SendError(res, 404, "no_release_found", std::format("no matching {} release found", tool));
      }

      const runner::ReleaseAsset asset = releases->front();  // newest first
      events_.Publish(event_prefix + ".started", {{"tag", asset.tag}});
      operations_.Run([this, asset, tool, event_prefix, install] {
        if (auto installed = install(config_, asset); !installed) {
          log::Error("{} install failed ({}): {}", tool, asset.tag, installed.error().message);
          events_.Publish(event_prefix + ".failed", FailedEvent({{"tag", asset.tag}}, installed.error()));
        } else {
          log::Info("installed {} {}", tool, asset.tag);
          events_.Publish(event_prefix + ".finished", {{"tag", asset.tag}});
        }
      });

      SendJson(res, {{"status", "downloading"}, {"tag", asset.tag}}, 202);
    });
  };
  register_cli_setup("/v1/epic/legendary/install", "legendary", "legendary", "epic.legendary.install",
                     epic::InstallLegendaryBinary);
  register_cli_setup("/v1/gog/setup", "gog", "gogdl", "gog.setup", gog::InstallGogBinary);
  register_cli_setup("/v1/amazon/setup", "amazon", "nile", "amazon.setup", amazon::InstallNileBinary);
  register_cli_setup("/v1/itch/setup", "itch", "butler", "itch.setup", itch::InstallButlerBinary);
  register_cli_setup("/v1/humble/setup", "humble", "humble-cli", "humble.setup", humble::InstallHumbleCliBinary);

  // --- epic -------------------------------------------------------------

  http_->Get("/v1/epic/legendary/status", [this](const Request&, Response& res) {
    const epic::LegendaryStatus status = epic::DetectLegendary(config_);
    SendJson(res, {{"installed", status.installed},
                  {"source", status.source},
                  {"path", status.path},
                  {"version", status.version}});
  });

  http_->Get("/v1/epic/status", [this](const Request&, Response& res) {
    const epic::EpicAuthStatus status = epic::Status(config_);
    SendJson(res, {{"legendary", {{"installed", status.legendary.installed},
                                  {"source", status.legendary.source},
                                  {"path", status.legendary.path},
                                  {"version", status.legendary.version}}},
                  {"authenticated", status.authenticated},
                  {"account", status.account},
                  {"login_url", epic::kLoginUrl}});
  });

  http_->Post("/v1/epic/auth", [this](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("code") || !body["code"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"code": "..."})");
    }
    if (auto logged_in = epic::Login(config_, body["code"]); !logged_in) {
      return SendError(res, 400, logged_in.error());
    }
    const epic::EpicAuthStatus status = epic::Status(config_);
    SendJson(res, {{"authenticated", status.authenticated}, {"account", status.account}});
  });

  http_->Post("/v1/epic/logout", [this](const Request&, Response& res) {
    if (auto logged_out = epic::Logout(config_); !logged_out) {
      return SendError(res, 400, logged_out.error());
    }
    SendJson(res, {{"status", "logged_out"}});
  });

  // --- gog ----------------------------------------------------------------

  http_->Get("/v1/gog/status", [this](const Request&, Response& res) {
    const gog::GogAuthStatus status = gog::Status(config_);
    SendJson(res, {{"gogdl", {{"installed", status.gogdl.installed},
                              {"source", status.gogdl.source},
                              {"path", status.gogdl.path},
                              {"version", status.gogdl.version}}},
                  {"authenticated", status.authenticated},
                  {"login_url", gog::kLoginUrl}});
  });

  http_->Post("/v1/gog/auth", [this](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("code") || !body["code"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"code": "..."})");
    }
    if (auto logged_in = gog::Login(config_, body["code"]); !logged_in) {
      return SendError(res, 400, logged_in.error());
    }
    const gog::GogAuthStatus status = gog::Status(config_);
    SendJson(res, {{"authenticated", status.authenticated}});
  });

  http_->Post("/v1/gog/logout", [this](const Request&, Response& res) {
    if (auto logged_out = gog::Logout(config_); !logged_out) {
      return SendError(res, 400, logged_out.error());
    }
    SendJson(res, {{"status", "logged_out"}});
  });

  // --- amazon -------------------------------------------------------------

  http_->Get("/v1/amazon/status", [this](const Request&, Response& res) {
    const amazon::AmazonAuthStatus status = amazon::Status(config_);
    SendJson(res, {{"nile", {{"installed", status.nile.installed},
                             {"source", status.nile.source},
                             {"path", status.nile.path},
                             {"version", status.nile.version}}},
                  {"authenticated", status.authenticated}});
  });

  http_->Post("/v1/amazon/login", [this](const Request&, Response& res) {
    const auto url = amazon::BeginLogin(config_);
    if (!url) return SendError(res, 409, url.error());
    SendJson(res, {{"url", *url}});
  });

  http_->Post("/v1/amazon/auth", [this](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("redirect") || !body["redirect"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"redirect": "..."})");
    }
    if (auto logged_in = amazon::FinishLogin(config_, body["redirect"]); !logged_in) {
      return SendError(res, 400, logged_in.error());
    }
    SendJson(res, {{"authenticated", true}});
  });

  http_->Post("/v1/amazon/logout", [this](const Request&, Response& res) {
    if (auto logged_out = amazon::Logout(config_); !logged_out) {
      return SendError(res, 400, logged_out.error());
    }
    SendJson(res, {{"status", "logged_out"}});
  });

  // POST <route> imports what the store's own tool reports as installed, as a job.
  // gog only looks under gog.install_root, since gogdl can't list installed games.
  http_->Post("/v1/epic/import", [import_job](const Request& req, Response& res) {
    import_job(req, res, std::type_identity<epic::EpicImporter>(), "epic", "Importing from Epic Games");
  });
  http_->Post("/v1/gog/import", [import_job](const Request& req, Response& res) {
    import_job(req, res, std::type_identity<gog::GogImporter>(), "gog", "Importing from GOG");
  });
  http_->Post("/v1/amazon/import", [import_job](const Request& req, Response& res) {
    import_job(req, res, std::type_identity<amazon::AmazonImporter>(), "amazon", "Importing from Amazon Games");
  });
  http_->Post("/v1/itch/import", [import_job](const Request& req, Response& res) {
    import_job(req, res, std::type_identity<itch::ItchImporter>(), "itch", "Importing from itch.io");
  });

  // --- store launchers --------------------------------------------------

  http_->Get("/v1/launchers", [this](const Request&, Response& res) {
    json list = json::array();
    for (const launchers::Launcher& launcher : launchers::All()) {
      const auto game = games_.Find(launchers::GameId(launcher));
      list.push_back({{"id", launcher.id},
                      {"name", launcher.name},
                      {"game_id", launchers::GameId(launcher)},
                      {"installed", launchers::Installed(games_, launcher)},
                      {"install_state", launchers::InstallState(launcher)},
                      {"interactive_install", launcher.interactive},
                      {"prefix", game ? game->data_dir : ""},
                      {"runner_ref", game ? game->runner_ref : ""},
                      {"error", game ? game->last_error : ""}});
    }
    SendJson(res, list);
  });

  http_->Post(R"(/v1/launchers/([^/]+)/install)", [this](const Request& req, Response& res) {
    const launchers::Launcher* launcher = launchers::Find(req.matches[1].str());
    if (!launcher) return SendError(res, 404, "launcher_not_found", "no such launcher");
    if (!launchers::BeginInstall(*launcher)) {
      return SendError(res, 409, "install_running", std::format("{} is already installing", launcher->name));
    }
    events_.Publish("launcher.install.started", {{"id", launcher->id}});
    operations_.Run([this, launcher] {
      const auto done = launchers::Install(config_, games_, *launcher);
      if (const auto stored = games_.Find(launchers::GameId(*launcher))) {
        events_.Publish("game.updated", Record(*stored));
      }
      if (!done) {
        events_.Publish("launcher.install.failed", FailedEvent({{"id", launcher->id}}, done.error()));
        return;
      }
      if (const auto imported = launchers::Import(config_, games_, events_, *launcher)) {
        for (const model::Game& game : imported->added_games) metadata_fetches_.Enqueue(config_, events_, game);
      }
      SyncDesktopEntries(config_, games_);
      events_.Publish("launcher.install.finished", {{"id", launcher->id}});
    });
    SendJson(res, {{"status", "installing"}, {"id", launcher->id}}, 202);
  });

  http_->Post(R"(/v1/launchers/([^/]+)/import)", [this](const Request& req, Response& res) {
    const launchers::Launcher* launcher = launchers::Find(req.matches[1].str());
    if (!launcher) return SendError(res, 404, "launcher_not_found", "no such launcher");
    StartJob(req, res, "import", launcher->id, "Importing from " + launcher->name,
             [this, launcher](JobRegistry::Progress&) -> Result<json> {
               const auto summary = launchers::Import(config_, games_, events_, *launcher);
               if (!summary) return std::unexpected(summary.error());
               SyncDesktopEntries(config_, games_);
               for (const model::Game& game : summary->added_games) metadata_fetches_.Enqueue(config_, events_, game);
               return json{{"added", summary->added}, {"updated", summary->updated}};
             });
  });

  http_->Post(R"(/v1/launchers/([^/]+)/open)", [this](const Request& req, Response& res) {
    const launchers::Launcher* launcher = launchers::Find(req.matches[1].str());
    if (!launcher) return SendError(res, 404, "launcher_not_found", "no such launcher");
    const json body = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) return SendError(res, 400, "invalid_body", "expected a JSON object");
    const std::string action = body.value("action", std::string("launch"));
    if (action != "launch" && action != "install") {
      return SendError(res, 400, "invalid_action", "action must be launch or install");
    }
    model::Game target;
    target.source = "launcher";
    target.source_ref = launcher->id;
    if (const std::string ref = body.value("ref", std::string()); !ref.empty()) {
      target.source = launcher->id;
      target.source_ref = ref;
    }
    auto command = launchers::BuildCommand(config_, games_, target, action);
    if (!command) return SendError(res, 409, command.error());
    if (auto spawned = runner::SpawnDetached(*command); !spawned) {
      return SendError(res, 500, spawned.error());
    }
    SendJson(res, {{"status", "opened"}});
  });

  // --- itch -----------------------------------------------------------

  http_->Get("/v1/itch/status", [this](const Request&, Response& res) {
    const itch::ItchAuthStatus status = itch::Status(config_);
    SendJson(res, {{"butler", {{"installed", status.butler.installed},
                               {"source", status.butler.source},
                               {"path", status.butler.path},
                               {"version", status.butler.version}}},
                  {"authenticated", status.authenticated},
                  {"login_url", itch::kApiKeysUrl}});
  });

  http_->Post("/v1/itch/auth", [this](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("api_key") || !body["api_key"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"api_key": "..."})");
    }
    if (auto logged_in = itch::Login(config_, body["api_key"]); !logged_in) {
      return SendError(res, 400, logged_in.error());
    }
    SendJson(res, {{"authenticated", true}});
  });

  http_->Post("/v1/itch/logout", [this](const Request&, Response& res) {
    if (auto logged_out = itch::Logout(config_); !logged_out) {
      return SendError(res, 400, logged_out.error());
    }
    SendJson(res, {{"status", "logged_out"}});
  });

  // --- sources --------------------------------------------------------------

  http_->Get(R"(/v1/sources/([a-z0-9-]+)/removal)", [this](const Request& req, Response& res) {
    auto plan = library::PlanRemoval(config_, games_, req.matches[1].str());
    if (!plan) return SendError(res, 404, plan.error());
    json games = json::array();
    for (const library::RemovalGame& game : plan->games) {
      games.push_back({{"id", game.id}, {"name", game.name}, {"deletes", game.deletes}});
    }
    SendJson(res, {{"source", plan->source},
                   {"games", games},
                   {"launcher_dir", plan->launcher_dir},
                   {"kept", plan->kept},
                   {"signs_out", plan->signs_out}});
  });

  http_->Post(R"(/v1/sources/([a-z0-9-]+)/remove)", [this](const Request& req, Response& res) {
    const std::string source = req.matches[1].str();
    // Checked now, so an unknown source is a 404 rather than a failed job.
    if (auto plan = library::PlanRemoval(config_, games_, source); !plan) return SendError(res, 404, plan.error());
    StartJob(req, res, "remove_source", source, "Removing " + source,
             [this, source](JobRegistry::Progress&) -> Result<json> {
               auto removed = library::RemoveSource(config_, games_, events_, source);
               if (!removed) return std::unexpected(removed.error());
               SyncDesktopEntries(config_, games_);
               return json{{"removed", removed->removed}, {"problems", removed->problems}};
             });
  });

  const auto send_source_runner = [](Response& res, const Result<library::SourceRunner>& runner) {
    if (!runner) {
      const int status = runner.error().code == "launcher_not_installed" ? 409 : 400;
      return SendError(res, status, runner.error());
    }
    SendJson(res, {{"runner_ref", runner->runner_ref}, {"games", runner->games}, {"differing", runner->differing}});
  };

  http_->Get(R"(/v1/sources/([a-z0-9-]+)/runner)", [this, send_source_runner](const Request& req, Response& res) {
    send_source_runner(res, library::GetSourceRunner(config_, games_, req.matches[1].str()));
  });

  http_->Post(R"(/v1/sources/([a-z0-9-]+)/runner)", [this, send_source_runner](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    if (!body.is_object() || !body.contains("runner_ref") || !body["runner_ref"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"runner_ref": "kind:name", "apply_to_games"?: bool})");
    }
    const auto runner = library::SetSourceRunner(config_, games_, req.matches[1].str(),
                                                 body["runner_ref"].get<std::string>(),
                                                 body.value("apply_to_games", false));
    if (runner) {
      for (const std::string& id : runner->changed) {
        if (const auto game = games_.Find(id)) events_.Publish("game.updated", Record(*game));
      }
    }
    send_source_runner(res, runner);
  });

  http_->Get("/v1/itch/collections", [this](const Request&, Response& res) {
    auto collections = itch::ListCollections(config_);
    if (!collections) return SendError(res, 400, collections.error());
    json out = json::array();
    for (const itch::ItchCollection& collection : *collections) out.push_back(CollectionJson(collection));
    SendJson(res, std::move(out));
  });

  http_->Post("/v1/itch/collections", [this](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    if (!body.is_object() || !body.contains("link") || !body["link"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"link": "https://itch.io/c/<id>/..."})");
    }
    auto added = itch::AddCollection(config_, body["link"].get<std::string>());
    if (!added) return SendError(res, 400, added.error());
    SendJson(res, CollectionJson(*added), 201);
  });

  http_->Delete(R"(/v1/itch/collections/(\d+))", [this](const Request& req, Response& res) {
    std::int64_t id = 0;
    const std::string digits = req.matches[1].str();
    if (std::from_chars(digits.data(), digits.data() + digits.size(), id).ec != std::errc()) {
      return SendError(res, 400, "invalid_id", "that collection id is out of range");
    }
    SendResult(res, itch::RemoveCollection(config_, id));
  });

  // --- humble -------------------------------------------------------------

  http_->Get("/v1/humble/status", [this](const Request&, Response& res) {
    const humble::HumbleAuthStatus status = humble::Status(config_);
    SendJson(res, {{"humble_cli", {{"installed", status.humble_cli.installed},
                                   {"source", status.humble_cli.source},
                                   {"path", status.humble_cli.path},
                                   {"version", status.humble_cli.version}}},
                  {"authenticated", status.authenticated},
                  {"login_url", humble::kLoginUrl}});
  });

  http_->Post("/v1/humble/auth", [this](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("session_key") || !body["session_key"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"session_key": "..."})");
    }
    if (auto logged_in = humble::Login(config_, body["session_key"]); !logged_in) {
      return SendError(res, 400, logged_in.error());
    }
    SendJson(res, {{"authenticated", true}});
  });

  http_->Get("/v1/humble/library", [this](const Request&, Response& res) {
    auto bundles = humble::ListBundles(config_);
    if (!bundles) return SendError(res, 400, bundles.error());
    json out = json::array();
    for (const humble::BundleSummary& bundle : *bundles) {
      out.push_back({{"key", bundle.key}, {"name", bundle.name}, {"claimed", bundle.claimed}});
    }
    SendJson(res, std::move(out));
  });

  http_->Post("/v1/humble/download", [this](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("bundle_key") || !body["bundle_key"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"bundle_key": "...", "item_numbers": "..."})");
    }
    const std::string bundle_key = body["bundle_key"];
    const std::string item_numbers = body.contains("item_numbers") && body["item_numbers"].is_string()
                                         ? body["item_numbers"].get<std::string>()
                                         : std::string();
    // The key becomes a folder name and a humble-cli argument.
    if (bundle_key.empty() || !std::ranges::all_of(bundle_key, [](unsigned char c) { return std::isalnum(c); }) ||
        !std::ranges::all_of(item_numbers, [](unsigned char c) { return std::isdigit(c) || c == ',' || c == ' ' || c == '-'; })) {
      return SendError(res, 400, "invalid_body", "bundle_key must be letters and digits, and item_numbers digits, commas and dashes");
    }

    events_.Publish("humble.download.started", {{"bundle_key", bundle_key}});
    operations_.Run([this, bundle_key, item_numbers] {
      const Result<bool> result = humble::Download(config_, bundle_key, item_numbers);
      if (!result) {
        log::Error("humble download failed ({}): {}", bundle_key, result.error().message);
        events_.Publish("humble.download.failed", FailedEvent({{"bundle_key", bundle_key}}, result.error()));
      } else if (!*result) {
        log::Warn("humble download for {} had nothing to download (a redeemed key with no Humble-hosted "
                 "files, most likely)",
                 bundle_key);
        events_.Publish("humble.download.finished",
                       {{"bundle_key", bundle_key},
                        {"path", humble::DownloadDir(config_, bundle_key).string()},
                        {"downloaded", false}});
      } else {
        log::Info("humble download finished: {}", bundle_key);
        events_.Publish("humble.download.finished",
                       {{"bundle_key", bundle_key},
                        {"path", humble::DownloadDir(config_, bundle_key).string()},
                        {"downloaded", true}});
      }
    });

    SendJson(res, {{"status", "downloading"}, {"bundle_key", bundle_key},
                  {"path", humble::DownloadDir(config_, bundle_key).string()}},
            202);
  });

  // --- library (what the account owns, across sources) ------------------

  http_->Get("/v1/library", [this](const Request& req, Response& res) {
    const std::string source = req.has_param("source") ? req.get_param_value("source") : "";
    auto entries = library::ListCatalog(config_, games_, source);
    if (!entries) return SendError(res, 400, entries.error());
    json out = json::array();
    for (const library::CatalogEntry& entry : *entries) {
      out.push_back({{"source", entry.source},
                     {"ref", entry.ref},
                     {"title", entry.title},
                     {"installed", entry.installed},
                     {"game_id", entry.game_id},
                     {"play_seconds", entry.play_seconds},
                     {"owned", entry.owned}});
    }
    SendJson(res, std::move(out));
  });

  // Keyed by {source, ref}, since the title isn't tracked yet. "update" in the
  // event payload tells an update from an install.
  auto library_install_or_update = [this](const Request& req, Response& res, bool is_update) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("source") || !body["source"].is_string() ||
        !body.contains("ref") || !body["ref"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"source": "epic"|"steam"|"gog"|"itch"|"amazon", "ref": "..."})");
    }
    const std::string source = body["source"];
    const std::string ref = body["ref"];

    library::ILibrarySource* src = library::FindSource(source);
    if (src == nullptr) {
      return SendError(res, 400, "unknown_source", std::format("no installable source named \"{}\"", source));
    }
    if (!IsSafeRef(ref)) return SendError(res, 400, "invalid_ref", "that ref isn't a store id");

    events_.Publish("library.install.started", {{"source", source}, {"ref", ref}, {"update", is_update}});
    operations_.Run([this, src, source, ref, is_update] {
      const Result<void> result = is_update ? src->Update(config_, games_, events_, ref)
                                            : src->Install(config_, games_, events_, ref);
      if (!result) {
        log::Error("{} {} failed ({}): {}", source, is_update ? "update" : "install", ref, result.error().message);
        events_.Publish("library.install.failed",
                        FailedEvent({{"source", source}, {"ref", ref}, {"update", is_update}}, result.error()));
      } else {
        log::Info("{} {} finished: {}", source, is_update ? "update" : "install", ref);
        SyncDesktopEntries(config_, games_);
        if (const auto game = games_.Find(source + "-" + ref)) metadata_fetches_.Enqueue(config_, events_, *game);
        events_.Publish("library.install.finished", {{"source", source}, {"ref", ref}, {"update", is_update}});
      }
    });

    SendJson(res, {{"status", is_update ? "updating" : "installing"}, {"ref", ref}}, 202);
  };
  http_->Get("/v1/library/artwork", [this](const Request& req, Response& res) {
    const std::string source = req.has_param("source") ? req.get_param_value("source") : "";
    const std::string ref = req.has_param("ref") ? req.get_param_value("ref") : "";
    if (library::FindSource(source) == nullptr || !IsSafeRef(ref)) {
      return SendError(res, 400, "invalid_request", "expected ?source=<store>&ref=<ref>");
    }
    SendCachedArtwork(config_, source + "-" + ref, "cover", res);
  });

  http_->Post("/v1/library/artwork", [this](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    const auto text = [](const json& object, const char* key) {
      const auto found = object.find(key);
      return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
    };
    const std::string source = body.is_object() ? text(body, "source") : "";
    if (library::FindSource(source) == nullptr || !body.contains("titles") || !body["titles"].is_array()) {
      return SendError(res, 400, "invalid_body",
                       R"(expected {"source": "...", "titles": [{"ref": "...", "title": "..."}]})");
    }
    if (!config_.GetBool("metadata.enabled")) return SendJson(res, {{"queued", 0}});

    std::vector<model::Game> titles;
    for (const json& entry : body["titles"]) {
      if (!entry.is_object()) continue;
      model::Game title;
      title.source = source;
      title.source_ref = text(entry, "ref");
      title.name = text(entry, "title");
      title.id = source + "-" + title.source_ref;
      if (!IsSafeRef(title.source_ref) || title.name.empty() || art_index_.For(title.id).contains("cover")) continue;
      // How Fetch tells a Steam game apart.
      if (source == "steam") title.runner_ref = "steam:" + title.source_ref;
      titles.push_back(std::move(title));
    }
    SendJson(res, {{"queued", metadata_fetches_.EnqueueTitles(config_, events_, std::move(titles))}}, 202);
  });

  http_->Post("/v1/library/install", [library_install_or_update](const Request& req, Response& res) {
    library_install_or_update(req, res, false);
  });
  http_->Post("/v1/library/update", [library_install_or_update](const Request& req, Response& res) {
    library_install_or_update(req, res, true);
  });

  // --- desktop entries --------------------------------------------------

  http_->Get("/v1/desktop-entries/candidates", [this](const Request&, Response& res) {
    desktop::DesktopEntryScanner scanner(config_, games_, events_);
    auto candidates = scanner.ListCandidates();
    if (!candidates) return SendError(res, 404, candidates.error());
    json out = json::array();
    for (const auto& c : *candidates) out.push_back({{"id", c.id}, {"name", c.name}, {"icon", c.icon}});
    SendJson(res, std::move(out));
  });

  http_->Post("/v1/desktop-entries/import", [this](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    const auto listed = body.is_object() && body.contains("ids") ? StringList(body, "ids") : std::nullopt;
    if (!listed) return SendError(res, 400, "invalid_body", R"(expected {"ids": ["..."]})");
    const std::vector<std::string>& ids = *listed;

    desktop::DesktopEntryScanner scanner(config_, games_, events_);
    auto summary = scanner.Import(ids);
    if (!summary) return SendError(res, 404, summary.error());
    SyncDesktopEntries(config_, games_);
    for (const model::Game& game : summary->added_games) metadata_fetches_.Enqueue(config_, events_, game);
    SendJson(res, {{"added", summary->added}, {"updated", summary->updated}});
  });

  http_->Post("/v1/desktop-entries/sync", [this](const Request&, Response& res) {
    SyncDesktopEntries(config_, games_);
    SendJson(res, {{"ok", true}});
  });

  // --- manual add -----------------------------------------------------------

  http_->Post("/v1/games/manual", [this](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("install_path") || !body["install_path"].is_string() ||
        !body.contains("exe_path") || !body["exe_path"].is_string()) {
      return SendError(res, 400, "invalid_body",
                       R"(expected {"install_path": "...", "exe_path": "...", "name"?, "platform"?, "is_installer"?})");
    }
    const std::filesystem::path install_path = body["install_path"].get<std::string>();
    const std::string exe_path = body["exe_path"];
    const bool is_installer = body.value("is_installer", false);

    model::Platform platform;
    if (body.contains("platform") && body["platform"].is_string()) {
      platform = model::PlatformFromString(body["platform"].get<std::string>());
    } else {
      const std::string ext = strings::ToLower(std::filesystem::path(exe_path).extension().string());
      const bool windows = ext == ".exe" || ext == ".msi" || ext == ".bat" || ext == ".cmd";
      platform = windows ? model::Platform::Windows : model::Platform::Native;
    }

    model::Game game;
    const auto existing = games_.FindByInstallPath(install_path.string());
    if (existing) game = *existing;
    game.name = body.value("name", strings::CleanGameName(install_path.filename().string()));
    game.id = existing ? game.id : games_.NextId(game.name);
    game.source = "manual";
    game.install_path = install_path.string();
    game.exe_path = exe_path;
    game.args = body.value("args", std::string());
    game.platform = platform;
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;
    // An installer needs the prefix too: it installs the game into it.
    if (platform != model::Platform::Windows) {
      game.data_dir.clear();
    } else if (game.data_dir.empty()) {
      game.data_dir = library::PrefixDir(config_, game).string();
    }

    if (is_installer) {
      // An installer isn't the game, so it isn't provisioned or launchable yet.
      game.status = model::GameStatus::NeedsInstall;
      game.last_error = "This is an installer, not the game itself. Run it first, then point Mira at the "
                        "installed game.";
    } else {
      game.status = model::GameStatus::Ready;
      game.last_error.clear();
    }

    auto result = games_.Upsert(game);
    if (!result) return SendError(res, 500, result.error());

    if (game.status == model::GameStatus::Ready && game.platform == model::Platform::Windows) {
      const runner::RunnerRegistry provisioner(config_);
      const model::Game provisioned = provisioner.ProvisionGame(game);
      auto saved = games_.Update(game.id, [&](model::Game& g) {
        g.runner_ref = provisioned.runner_ref;
        g.status = provisioned.status;
        g.last_error = provisioned.last_error;
      });
      if (saved) game = *saved;
    }

    SyncDesktopEntries(config_, games_);
    if (!existing) metadata_fetches_.Enqueue(config_, events_, game);
    events_.Publish(existing ? "game.updated" : "game.added", Record(game));
    SendJson(res, Record(game));
  });

  // --- launching ------------------------------------------------------------

  http_->Post(R"(/v1/games/([^/]+)/launch)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    if (game->status == model::GameStatus::Broken &&
        library::RetryBrokenProvisioning(config_, games_, events_, game->id)) {
      game = games_.Find(game->id);
      if (!game) return SendError(res, 404, "game_not_found", "no such game");
    }
    if (game->status == model::GameStatus::NeedsInstall) {
      return SendError(res, 409, "needs_install",
                       game->last_error.empty() ? std::format("\"{}\" needs installing before it can launch", game->id)
                                                : game->last_error);
    }
    if (game->status != model::GameStatus::Ready) {
      return SendError(res, 409, "not_ready",
                       std::format("\"{}\" is {}, not ready to launch", game->id,
                                  model::ToString(game->status)));
    }

    const auto reservation = supervisor_.Reserve(game->id);
    if (!reservation) {
      return SendError(res, 409, "already_running", std::format("\"{}\" is already running", game->id));
    }

    const config::Resolver resolver(config_, game->overrides);
    const std::string pre_script = resolver.GetString("launch.pre_script");
    const std::string post_script = resolver.GetString("launch.post_script");

    // A launcher game is started by its launcher, which keeps running after the
    // game exits; the game's own processes are tracked.
    if (launchers::ForGame(*game)) {
      auto command = launchers::BuildCommand(config_, games_, *game);
      if (!command) return SendError(res, 409, command.error());
      if (auto ran = RunPreScriptInline(pre_script); !ran) {
        return SendError(res, 409, ran.error());
      }
      ApplyLaunchEnv(*command, resolver.GetStringArray("launch.env"));
      if (auto spawned = runner::SpawnDetached(*command); !spawned) {
        return SendError(res, 500, spawned.error());
      }
      [[maybe_unused]] auto _ =
          games_.Update(game->id, [](model::Game& g) { g.last_played_at = model::NowSeconds(); });
      events_.Publish("game.launched", {{"id", game->id}, {"via", "launcher"}, {"tracked", true}});
      if (auto started = supervisor_.TrackLauncherLaunch(*game, launchers::WindowsDir(*game),
                                                         config_.GetInt("launchers.detect_timeout_s"), post_script);
          !started) {
        log::Warn("couldn't start tracking {}: {}", game->id, started.error().message);
      }
      return SendJson(res, {{"status", "launched_via_launcher"}, {"tracked", true}});
    }

    // Steam games default to a steam://rungameid handoff for overlay and achievements.
    // Mira can't waitpid() that process; steam.track_process finds it in /proc instead.
    if (game->runner_ref.starts_with("steam:")) {
      if (resolver.GetString("steam.launch_mode") == "steam") {
        if (auto ran = RunPreScriptInline(pre_script); !ran) {
          return SendError(res, 409, ran.error());
        }
        const std::string appid = game->runner_ref.substr(std::string_view("steam:").size());
        Command command;
        command.argv = {"steam", std::format("steam://rungameid/{}", appid)};
        if (auto spawned = runner::SpawnDetached(command); !spawned) {
          return SendError(res, 500, spawned.error());
        }
        [[maybe_unused]] auto _ =
            games_.Update(game->id, [](model::Game& g) { g.last_played_at = model::NowSeconds(); });
        const bool track = resolver.GetBool("steam.track_process");
        events_.Publish("game.launched",
                        {{"id", game->id}, {"via", "steam"}, {"tracked", track}});
        if (track) {
          if (auto started = supervisor_.TrackSteamLaunch(*game, appid, post_script); !started) {
            log::Warn("couldn't start tracking {}: {}", game->id, started.error().message);
          }
        }
        return SendJson(res, {{"status", "launched_via_steam"}, {"tracked", track}});
      }
    }

    const runner::RunnerRegistry registry(config_);
    auto resolved = registry.Resolve(registry.ResolveRef(*game));
    if (!resolved) return SendError(res, 400, resolved.error());

    if (game->runner_ref.empty() && resolved->build && config_.GetBool("launch.pin_runner")) {
      const std::string pinned = std::format("{}:{}", resolved->runner->kind(), resolved->build->name);
      game->runner_ref = pinned;
      [[maybe_unused]] auto _ = games_.Update(game->id, [&](model::Game& g) { g.runner_ref = pinned; });
    }

    auto command = resolved->runner->BuildCommand(*game, resolved->build);
    if (!command) return SendError(res, 400, command.error());
    // Otherwise the spawned child's chdir fails and it exits 127 before anything is logged.
    if (std::error_code ec; !command->cwd.empty() && !std::filesystem::is_directory(command->cwd, ec)) {
      return SendError(res, 409,
                       Error{"working_dir_missing",
                             std::format("the folder the game starts in, \"{}\", doesn't exist", command->cwd.string()),
                             "Check the game's folder is still there, or choose its executable again.",
                             Fix::Game(game->id, "exe")});
    }

    const std::vector<std::string> wrappers = resolver.GetStringArray("command_wrappers");
    if (auto checked = CheckCommandWrappers(wrappers); !checked) {
      return SendError(res, 400, checked.error());
    }
    ApplyLaunchEnv(*command, resolver.GetStringArray("launch.env"));
    ApplyCommandWrappers(*command, wrappers);

    // A Windows "game" that turns out to be an installer is caught at exit (CheckForInstall).
    // Only for an existing prefix: a new one's own Program Files would all look installed.
    if (std::error_code ec; game->platform == model::Platform::Windows && !game->data_dir.empty() &&
                            std::filesystem::exists(std::filesystem::path(game->data_dir) / "drive_c", ec)) {
      auto folders = library::InstallFolders(config_, game->data_dir);
      const std::lock_guard lock(install_watch_mutex_);
      install_watch_[game->id] = std::move(folders);
    }

    // mira-run owns the session so it survives mirad dying. Without it the game is
    // launched directly, with no session record.
    const auto mira_run = runner::ResolveSiblingBinary(OwnBinaryDir(), "mira-run");
    if (!mira_run) {
      log::Warn("mira-run not found; launching {} directly with no session recording", game->id);
      if (auto ran = RunPreScriptInline(pre_script); !ran) {
        return SendError(res, 409, ran.error());
      }
      if (auto launched = supervisor_.Launch(*game, *command, post_script); !launched) {
        return SendError(res, 409, launched.error());
      }
      return SendJson(res, {{"status", "running"}, {"tracked", true}});
    }

    const int pre_timeout_s = static_cast<int>(resolver.GetInt("launch.pre_timeout_s"));
    const std::filesystem::path sessions_dir = games_.Dir() / "sessions";
    const std::filesystem::path log_file = games_.Dir() / "logs" / std::format("{}.log", game->id);
    Command wrapped;
    wrapped.env = command->env;
    wrapped.cwd = command->cwd;
    wrapped.argv = {*mira_run,         "--game-id",       game->id,
                    "--session-dir",  sessions_dir.string(), "--log-file", log_file.string(),
                    "--log-max-mb",   std::to_string(resolver.GetInt("launch.log_max_mb")),
                    "--status-fd",    "3",
                    "--pre-timeout",  std::to_string(pre_timeout_s),
                    "--post-timeout", std::to_string(resolver.GetInt("launch.post_timeout_s"))};
    if (!pre_script.empty()) {
      wrapped.argv.push_back("--pre");
      wrapped.argv.push_back(pre_script);
    }
    if (!post_script.empty()) {
      wrapped.argv.push_back("--post");
      wrapped.argv.push_back(post_script);
    }
    if (resolver.GetBool("launch.gamemode")) wrapped.argv.push_back("--gamemode");
    wrapped.argv.push_back("--");
    wrapped.argv.insert(wrapped.argv.end(), command->argv.begin(), command->argv.end());

    int status_fd = -1;
    auto wrapper_pid = runner::SpawnDetachedWithStatus(wrapped, status_fd);
    if (!wrapper_pid) return SendError(res, 500, wrapper_pid.error());

    const WrapperStatus status = ReadWrapperStatus(status_fd, pre_timeout_s + 10);
    ::close(status_fd);
    // WNOHANG: on a read timeout mira-run may still be hung, and this thread must not
    // block on it.
    int wait_status = 0;
    ::waitpid(*wrapper_pid, &wait_status, WNOHANG);

    if (status.read_timed_out) {
      return SendError(res, 500, "wrapper_unresponsive", "mira-run did not respond in time");
    }
    if (status.code == "pre_failed") {
      return SendError(res, 409,
                       Error{"pre_launch_failed", "the pre-launch script failed: " + status.detail,
                             "Its output is in the game's log.", Fix::Game(game->id, "log")});
    }
    if (status.code == "pre_timeout") {
      return SendError(res, 409,
                       Error{"pre_launch_timeout",
                             std::format("the pre-launch script didn't finish within {}s", pre_timeout_s),
                             "Make the script finish sooner, or allow it more time.",
                             Fix::Setting("launch.pre_timeout_s")});
    }
    if (!status.ok) {
      return SendError(res, 500, "wrapper_failed", std::format("unexpected mira-run status: {}", status.code));
    }

    if (auto launched = supervisor_.LaunchWrapped(*game, *wrapper_pid, std::filesystem::path(status.detail));
        !launched) {
      return SendError(res, 409, launched.error());
    }
    SendJson(res, {{"status", "running"}, {"tracked", true}});  // always true: Mira spawned it
  });

  http_->Post(R"(/v1/games/([^/]+)/stop)", [this](const Request& req, Response& res) {
    if (auto stopped = supervisor_.Stop(req.matches[1]); !stopped) {
      // A client that missed the exit gets told it's stopped instead of an error.
      if (stopped.error().code == "not_running") {
        if (const auto game = games_.Find(req.matches[1])) {
          json event = Record(*game);
          event["state"] = "idle";
          events_.Publish("game.state", std::move(event));
          return SendJson(res, {{"status", "not_running"}});
        }
      }
      return SendError(res, 409, stopped.error());
    }
    SendJson(res, {{"status", "stopping"}});
  });

  http_->Post(R"(/v1/games/([^/]+)/run)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("exe_path") || !body["exe_path"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"exe_path": "...", "args": "..."})");
    }
    const std::string exe_path = body["exe_path"];
    const std::string args = body.value("args", std::string());

    // A game with no usable prefix yet gets one now.
    std::error_code ec;
    const bool needs_provisioning = game->platform == model::Platform::Windows &&
        (game->runner_ref.empty() || !std::filesystem::exists(std::filesystem::path(game->data_dir) / "drive_c", ec));
    if (needs_provisioning) {
      const runner::RunnerRegistry provisioner(config_);
      const model::Game provisioned = provisioner.ProvisionGame(*game);
      auto saved = games_.Update(game->id, [&](model::Game& g) {
        g.runner_ref = provisioned.runner_ref;
        if (provisioned.status == model::GameStatus::Broken) {
          g.status = model::GameStatus::Broken;
          g.last_error = provisioned.last_error;
        } else {
          g.last_error.clear();
        }
      });
      if (!saved) return SendError(res, 404, saved.error());
      game = *saved;
      if (game->status == model::GameStatus::Broken) {
        return SendError(res, 409,
                         Error{"provision_failed", "couldn't set up this game's Wine prefix: " + game->last_error,
                               "The runner may be broken. Try a different one.", Fix::Runners()});
      }
    }

    const runner::RunnerRegistry registry(config_);
    auto resolved = registry.Resolve(registry.ResolveRef(*game));
    if (!resolved) return SendError(res, 400, resolved.error());

    if (game->runner_ref.empty() && resolved->build && config_.GetBool("launch.pin_runner")) {
      const std::string pinned = std::format("{}:{}", resolved->runner->kind(), resolved->build->name);
      game->runner_ref = pinned;
      [[maybe_unused]] auto _ = games_.Update(game->id, [&](model::Game& g) { g.runner_ref = pinned; });
    }

    model::Game run_as = *game;
    run_as.exe_path = exe_path;
    run_as.args = args;

    auto command = resolved->runner->BuildCommand(run_as, resolved->build);
    if (!command) return SendError(res, 400, command.error());
    const config::Resolver resolver(config_, game->overrides);
    const std::vector<std::string> wrappers = resolver.GetStringArray("command_wrappers");
    if (auto checked = CheckCommandWrappers(wrappers); !checked) {
      return SendError(res, 400, checked.error());
    }
    ApplyLaunchEnv(*command, resolver.GetStringArray("launch.env"));
    ApplyCommandWrappers(*command, wrappers);

    if (auto launched = supervisor_.Launch(*game, *command); !launched) {
      return SendError(res, 409, launched.error());
    }
    SendJson(res, {{"status", "running"}});
  });

  // Describes a game's installer, or the file at ?path=.
  http_->Get(R"(/v1/games/([^/]+)/installer)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    if (req.has_param("path")) game->exe_path = req.get_param_value("path");
    const auto info = library::DescribeInstaller(config_, *game);
    if (!info) return SendError(res, 404, info.error());
    SendJson(res, {{"path", info->path.string()},
                   {"size_bytes", info->size_bytes},
                   {"format", library::ToString(info->format)},
                   {"silent", info->silent},
                   {"silent_args", info->silent_args}});
  });

  http_->Get(R"(/v1/games/([^/]+)/install/progress)", [this](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!games_.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
    const auto progress = library::Progress(id);
    if (!progress) return SendJson(res, {{"state", "idle"}});
    SendJson(res, {{"state", progress->state},
                   {"mode", progress->mode},
                   {"started_at", progress->started_at},
                   {"finished_at", progress->finished_at},
                   {"error", progress->error},
                   {"bytes_written", progress->bytes_written}});
  });

  http_->Post(R"(/v1/games/([^/]+)/install)", [this](const Request& req, Response& res) {
    const auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    const json body = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (body.is_discarded() || !body.is_object()) return SendError(res, 400, "invalid_body", "expected a JSON object");
    const bool interactive = body.value("interactive", false);
    std::optional<std::filesystem::path> installer;
    if (body.contains("installer")) {
      if (!body["installer"].is_string()) return SendError(res, 400, "invalid_body", "installer must be a string");
      installer = std::filesystem::path(game->install_path) / body["installer"].get<std::string>();
      if (!paths::IsWithin(*installer, {std::filesystem::path(game->install_path)})) {
        return SendError(res, 400, "invalid_body", "installer must be inside the game's folder");
      }
      std::error_code ec;
      if (!std::filesystem::is_regular_file(*installer, ec)) {
        return SendError(res, 404,
                         Error{"installer_missing", std::format("there's no installer at {}", installer->string()),
                               "Pick the installer again."});
      }
    }
    const bool installable = game->status == model::GameStatus::NeedsInstall ||
                             (installer && game->status == model::GameStatus::Broken);
    if (!installable) return SendError(res, 409, "not_needs_install", "game isn't waiting on an installer");
    if (!library::BeginInstall(game->id)) {
      return SendError(res, 409, "install_running", "an install is already running for this game");
    }

    events_.Publish("game.install.started", {{"id", game->id}});
    operations_.Run([this, id = game->id, interactive, installer] {
      const auto done = library::Install(config_, games_, id,
                                         interactive ? library::InstallMode::kInteractive : library::InstallMode::kAuto,
                                         installer);
      if (done) {
        SyncDesktopEntries(config_, games_);
        events_.Publish("game.updated", Record(*done));
        events_.Publish("game.install.finished", {{"id", id}});
      } else {
        if (const auto stored = games_.Find(id)) events_.Publish("game.updated", Record(*stored));
        events_.Publish("game.install.failed", FailedEvent({{"id", id}}, done.error()));
      }
    });
    SendJson(res, {{"status", "installing"}, {"id", game->id}}, 202);
  });

  http_->Post(R"(/v1/games/([^/]+)/finish-install)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    const auto needs_exe = [&](std::string message) {
      SendError(res, 409,
                Error{"no_executable", std::move(message),
                      "Choose the installed game's executable, then mark it installed again.",
                      Fix::Game(game->id, "exe")});
    };
    // Optionally adopting what the game's installer put in its prefix (game.install_detected).
    if (!req.body.empty()) {
      const json body = json::parse(req.body, nullptr, false);
      if (!body.is_object()) {
        return SendError(res, 400, "invalid_body", R"(expected {"install_path"?: "...", "exe_path"?: "..."})");
      }
      if (const std::string install_path = core::JsonString(body, "install_path"); !install_path.empty()) {
        if (!paths::IsWithin(install_path, {game->data_dir})) {
          return SendError(res, 400, "invalid_install_path", "the install folder must be inside the game's prefix");
        }
        if (supervisor_.IsRunning(game->id)) return SendError(res, 409, GameRunningError(game->id));
        const library::Detector::Result detected =
            library::Detector(library::SettingsFromConfig(config_)).Detect(install_path);
        game->install_path = install_path;
        game->working_dir.clear();
        game->candidates = detected.candidates;
        game->confidence = detected.confidence;
      }
      if (const std::string exe_path = core::JsonString(body, "exe_path"); !exe_path.empty()) {
        game->exe_path = exe_path;
      }
      for (model::Candidate& candidate : game->candidates) candidate.chosen = candidate.rel_path == game->exe_path;
    }
    if (game->exe_path.empty()) return needs_exe("this game has no executable set");
    const bool still_installer = std::ranges::any_of(game->candidates, [&](const model::Candidate& c) {
      return c.is_installer && c.rel_path == game->exe_path;
    });
    if (still_installer) return needs_exe("the game's executable is still the installer");
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(game->install_path) / game->exe_path, ec)) {
      return needs_exe("the game's executable isn't in its install folder");
    }
    auto result = games_.Update(game->id, [&](model::Game& g) {
      g.install_path = game->install_path;
      g.working_dir = game->working_dir;
      g.exe_path = game->exe_path;
      g.candidates = game->candidates;
      g.confidence = game->confidence;
      g.status = model::GameStatus::Ready;
      g.last_error.clear();
    });
    if (!result) return SendStoreError(res, result.error());
    SyncDesktopEntries(config_, games_);
    events_.Publish("game.updated", Record(*result));
    SendJson(res, Record(*result));
  });

  http_->Post(R"(/v1/games/([^/]+)/relocate)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    library::RelocateRequest request;
    if (!req.body.empty()) {
      const json body = json::parse(req.body, nullptr, false);
      if (body.is_discarded() || !body.is_object()) {
        return SendError(res, 400, "invalid_body", R"(expected {"install_path"?: "...", "data_dir"?: "..."})");
      }
      request.only_given = true;
      if (body.contains("install_path") && body["install_path"].is_string()) {
        request.install_path = std::filesystem::path(body["install_path"].get<std::string>());
      }
      if (body.contains("data_dir") && body["data_dir"].is_string()) {
        request.data_dir = std::filesystem::path(body["data_dir"].get<std::string>());
      }
    }

    if (supervisor_.IsRunning(game->id)) return SendError(res, 409, GameRunningError(game->id));

    StartJob(req, res, "relocate", game->id, "Moving " + game->name,
             [this, game = *game, request](JobRegistry::Progress&) -> Result<json> {
               auto folders_lock = games_.LockFolders();
               auto relocated = library::Relocate(config_, game, request);
               if (!relocated) return std::unexpected(relocated.error());
               auto saved = games_.Update(game.id, [&](model::Game& g) {
                 g.install_path = relocated->install_path;
                 g.data_dir = relocated->data_dir;
                 g.updated_at = model::NowSeconds();
               });
               folders_lock.unlock();
               if (!saved) return std::unexpected(saved.error());
               SyncDesktopEntries(config_, games_);
               json record = Record(*saved);
               events_.Publish("game.updated", record);
               return record;
             });
  });

  http_->Post(R"(/v1/games/([^/]+)/tricks)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("verb") || !body["verb"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"verb": "..."})");
    }
    const std::string verb = body["verb"];
    const std::string id = game->id;

    events_.Publish("tricks.started", {{"id", id}, {"verb", verb}});
    tricks_queue_.Run([this, id, verb] {
      const runner::RunnerRegistry registry(config_);
      const auto game = games_.Find(id);
      if (!game) {  // removed while queued
        const Error removed{"game_not_found", "the game was removed", {}, {}};
        events_.Publish("tricks.failed", FailedEvent({{"id", id}, {"verb", verb}}, removed));
        return;
      }
      if (auto ran = runner::RunTricksVerb(registry, *game, verb); !ran) {
        log::Error("winetricks {} failed for {}: {}", verb, id, ran.error().message);
        events_.Publish("tricks.failed", FailedEvent({{"id", id}, {"verb", verb}}, ran.error()));
      } else {
        events_.Publish("tricks.finished", {{"id", id}, {"verb", verb}});
      }
    });
    SendJson(res, {{"status", "running"}, {"verb", verb}}, 202);
  });

  // --- metadata ---------------------------------------------------------

  http_->Get(R"(/v1/games/([^/]+)/metadata)", [this](const Request& req, Response& res) {
    if (!games_.Find(req.matches[1])) return SendError(res, 404, "game_not_found", "no such game");
    const std::filesystem::path file = metadata::MetadataFile(config_, req.matches[1]);
    std::ifstream in(file);
    if (!in) return SendError(res, 404, "metadata_not_found", "no metadata cached for this game yet");
    std::ostringstream buffer;
    buffer << in.rdbuf();
    res.set_content(buffer.str(), "application/json");
  });

  http_->Get(R"(/v1/games/([^/]+)/artwork)", [this](const Request& req, Response& res) {
    if (!games_.Find(req.matches[1])) return SendError(res, 404, "game_not_found", "no such game");
    // The "cover" slot is stored as "artwork".
    SendCachedArtwork(config_, req.matches[1], req.has_param("type") ? req.get_param_value("type") : "cover", res);
  });

  // Takes a candidate id, never a URL, so the daemon can't be made to fetch an
  // arbitrary address.
  http_->Post(R"(/v1/games/([^/]+)/artwork)", [this](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!games_.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
    if (!req.has_param("type")) return SendError(res, 400, "missing_type", "?type= is required");
    const std::string slot = req.get_param_value("type");
    const json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.value("candidate_id", json()).is_number_integer()) {
      return SendError(res, 400, "invalid_json", "body must be {\"candidate_id\": <id>}");
    }
    const std::int64_t candidate_id = body["candidate_id"].get<std::int64_t>();
    artwork_selects_.Run([this, id, slot, candidate_id] {
      if (auto selected = metadata::SelectArtwork(config_, id, slot, candidate_id); !selected) {
        events_.Publish("game.artwork_select_failed", FailedEvent({{"id", id}, {"type", slot}}, selected.error()));
      } else {
        events_.Publish("game.artwork_selected", {{"id", id}, {"type", slot}});
      }
    });
    SendJson(res, {{"status", "selecting"}}, 202);
  });

  http_->Post(R"(/v1/games/([^/]+)/artwork/candidates)", [this](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!games_.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
    if (!req.has_param("type")) return SendError(res, 400, "missing_type", "?type= is required");
    const std::string slot = req.get_param_value("type");
    int page = 0;
    const std::string raw = req.has_param("page") ? req.get_param_value("page") : "0";
    if (std::from_chars(raw.data(), raw.data() + raw.size(), page).ec != std::errc() || page < 0) {
      return SendError(res, 400, "invalid_page", "?page= must be 0 or more");
    }
    // Echoed back, so a caller can tell its answer from a replayed one.
    const std::string request = req.has_param("request") ? req.get_param_value("request") : "";
    artwork_thumbs_.Run([this, id, slot, page, request] {
      json event = {{"id", id}, {"type", slot}, {"page", page}, {"request", request}};
      if (auto fetched = metadata::FetchCandidatePage(config_, id, slot, page); fetched) {
        event.update(*fetched);
      } else {
        event = FailedEvent(std::move(event), fetched.error());
      }
      events_.Publish("game.artwork_candidates_ready", event);
    });
    SendJson(res, {{"status", "fetching"}}, 202);
  });

  // Cached previews are ready at once; the rest are fetched in the background.
  http_->Post(R"(/v1/games/([^/]+)/artwork/thumbs)", [this](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!games_.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
    if (!req.has_param("type")) return SendError(res, 400, "missing_type", "?type= is required");
    const std::string slot = req.get_param_value("type");
    const json body = json::parse(req.body, nullptr, false);
    const json ids = body.is_object() ? body.value("candidate_ids", json()) : json();
    if (!ids.is_array() || ids.empty() || ids.size() > 64 ||
        !std::ranges::all_of(ids, [](const json& value) { return value.is_number_integer(); })) {
      return SendError(res, 400, "invalid_json", "body must be {\"candidate_ids\": [<id>, ...]}, 1 to 64 ids");
    }
    const std::vector<std::int64_t> candidate_ids = ids.get<std::vector<std::int64_t>>();
    artwork_thumbs_.Run([this, id, slot, candidate_ids] {
      json event = {{"id", id}, {"type", slot}};
      if (auto batch = metadata::FetchCandidateThumbs(config_, id, slot, candidate_ids); batch) {
        event["ready"] = batch->ready;
        event["failed"] = batch->failed;
      } else {
        event["ready"] = json::array();
        event["failed"] = candidate_ids;
        event = FailedEvent(std::move(event), batch.error());
      }
      events_.Publish("game.artwork_thumbs_ready", event);
    });
    SendJson(res, {{"status", "fetching"}}, 202);
  });

  http_->Get(R"(/v1/games/([^/]+)/artwork/thumb)", [this](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!games_.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
    const std::string slot = req.has_param("type") ? req.get_param_value("type") : "cover";
    std::int64_t candidate_id = 0;
    const std::string raw = req.has_param("candidate_id") ? req.get_param_value("candidate_id") : "";
    if (std::from_chars(raw.data(), raw.data() + raw.size(), candidate_id).ec != std::errc() || raw.empty()) {
      return SendError(res, 400, "missing_candidate_id", "?candidate_id= is required");
    }
    const std::filesystem::path file = metadata::CandidateThumbFile(config_, id, slot, candidate_id);
    std::ifstream in(file, std::ios::binary);
    if (file.empty() || !in) return SendError(res, 404, "thumb_not_cached", "no preview cached for that candidate");
    std::ostringstream buffer;
    buffer << in.rdbuf();
    res.set_content(buffer.str(), SniffImageType(buffer.str()));
  });

  http_->Delete("/v1/artwork/thumbs", [this](const Request&, Response& res) {
    metadata::ClearCandidateThumbs(config_);
    res.status = 204;
  });

  // force=true: an explicit refresh works even with metadata.enabled off.
  http_->Post(R"(/v1/games/([^/]+)/metadata/refresh)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    // Only user-initiated refreshes report failure as a notification.
    const bool announce = req.get_param_value("announce") == "1";
    metadata_fetches_.Enqueue(config_, events_, *game, /*force=*/true, announce);
    SendJson(res, {{"status", "fetching"}}, 202);
  });

  http_->Get(R"(/v1/games/([^/]+)/metadata/matches)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    const std::string query = req.has_param("q") ? req.get_param_value("q") : game->name;
    auto matches = metadata::SearchSteamGridDb(config_, query);
    if (!matches) return SendError(res, 502, matches.error());
    const std::int64_t chosen = config::Resolver(config_, game->overrides).GetInt("metadata.steamgriddb_id");
    SendJson(res, {{"query", query}, {"chosen", chosen}, {"matches", *matches}});
  });

  http_->Post(R"(/v1/games/([^/]+)/metadata/wrong-match)", [this](const Request& req, Response& res) {
    auto game = games_.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    auto matches = metadata::SearchSteamGridDb(config_, game->name);
    if (!matches) return SendError(res, 502, matches.error());
    const std::int64_t current = config::Resolver(config_, game->overrides).GetInt("metadata.steamgriddb_id");
    std::size_t next = 1;  // no choice yet: the top match was in use
    for (std::size_t i = 0; i < matches->size(); ++i) {
      if ((*matches)[i].value("id", std::int64_t{0}) == current) next = i + 1;
    }
    if (next >= matches->size()) {
      return SendError(res, 409, "no_more_matches",
                       "no other SteamGridDB match for this name -- pick one with ?q= on .../metadata/matches");
    }
    const json& match = (*matches)[next];
    const json patch = {{"metadata.steamgriddb_id", match.value("id", std::int64_t{0})}};
    auto updated = games_.Update(game->id, [&](model::Game& g) { ApplyOverridesPatch(g, patch); });
    if (!updated) return SendError(res, 500, updated.error());
    metadata_fetches_.Enqueue(config_, events_, *updated, /*force=*/true, /*announce=*/true);
    SendJson(res, {{"status", "fetching"}, {"match", match}}, 202);
  });

  http_->Post(R"(/v1/games/([^/]+)/metadata/match)", [this](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("steamgriddb_id") || !body["steamgriddb_id"].is_number_integer() ||
        body["steamgriddb_id"].get<std::int64_t>() < 0) {
      return SendError(res, 400, "invalid_body", R"(expected {"steamgriddb_id": <id, or 0 for the top match>})");
    }
    const std::int64_t id = body["steamgriddb_id"];
    const json patch = {{"metadata.steamgriddb_id", id == 0 ? json(nullptr) : json(id)}};
    auto game = games_.Update(req.matches[1], [&](model::Game& g) { ApplyOverridesPatch(g, patch); });
    if (!game) return SendError(res, 404, game.error());
    metadata_fetches_.Enqueue(config_, events_, *game, /*force=*/true, /*announce=*/true);
    SendJson(res, {{"status", "fetching"}, {"steamgriddb_id", id}}, 202);
  });

  // POST /v1/games/{id}/metadata/refresh for many games in one request, unannounced, as a job.
  http_->Post("/v1/games/metadata/refresh", [this](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    const auto ids = body.is_object() ? StringList(body, "ids") : std::nullopt;
    if (!ids) return SendError(res, 400, "invalid_body", R"(expected {"ids": [...]})");
    std::vector<model::Game> games;
    for (const std::string& id : *ids) {
      if (auto game = games_.Find(id)) games.push_back(std::move(*game));
    }
    StartJob(req, res, "metadata", "", "Refreshing metadata",
             [this, games = std::move(games)](JobRegistry::Progress& progress) { return RefreshMetadata(games, progress); });
  });

  http_->Post("/v1/games/metadata/refresh-missing", [this](const Request& req, Response& res) {
    std::vector<model::Game> games;
    for (const model::Game& game : games_.All()) {
      if (!art_index_.For(game.id).contains("cover")) games.push_back(game);
    }
    StartJob(req, res, "metadata", "", "Fetching missing cover art",
             [this, games = std::move(games)](JobRegistry::Progress& progress) { return RefreshMetadata(games, progress); });
  });

  // --- runners --------------------------------------------------------------

  http_->Get("/v1/runners", [this](const Request&, Response& res) {
    const runner::RunnerRegistry registry(config_);
    json out = json::array();
    for (const model::RunnerBuild& build : registry.DiscoverAll()) {
      json entry = model::ToJson(build);
      entry["label"] = runner::BuildLabel(build.kind, build.name);
      if (build.kind == "proton" || build.kind == "wine") {
        const std::filesystem::path dir = BuildDir(build);
        entry["removable"] = paths::IsWithin(dir, RunnerRoots(config_, build.kind));
        const auto family = runner::FamilyOfBuild(config_, build.kind, build.name, dir.filename().string());
        entry["source"] = family ? family->id : "";
      }
      out.push_back(std::move(entry));
    }
    SendJson(res, std::move(out));
  });

  http_->Get("/v1/runners/sources", [this](const Request& req, Response& res) {
    const std::string kind = req.has_param("kind") ? req.get_param_value("kind") : "";
    json out = json::array();
    for (const runner::RunnerFamily& family : runner::Families(config_, kind)) {
      out.push_back({{"id", family.id}, {"kind", family.kind}, {"label", family.label}});
    }
    SendJson(res, std::move(out));
  });

  http_->Get("/v1/runners/catalog", [this](const Request& req, Response& res) {
    const std::string kind = req.has_param("kind") ? req.get_param_value("kind") : "proton";
    auto family = FamilyFor(config_, kind, req.has_param("source") ? req.get_param_value("source") : "");
    if (!family) return SendError(res, 404, family.error());
    auto releases = runner::ListFamilyReleases(*family);
    if (!releases) return SendError(res, 502, releases.error());
    const runner::RunnerRegistry registry(config_);
    const std::vector<model::RunnerBuild> installed = BuildsOfKind(registry, kind);
    json out = json::array();
    for (const auto& r : *releases) {
      out.push_back({{"tag", r.tag}, {"name", runner::ReleaseName(kind, r)},
                     {"label", runner::BuildLabel(kind, runner::ReleaseName(kind, r))}, {"source", family->id},
                     {"asset_name", r.asset_name}, {"size_bytes", r.size_bytes},
                     {"published_at", r.published_at}, {"has_checksum", !r.checksum_url.empty()},
                     {"installed", HasInstalled(installed, r)}});
    }
    SendJson(res, std::move(out));
  });

  http_->Post("/v1/runners/download", [this](const Request& req, Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("kind") || !body.contains("tag")) {
      return SendError(res, 400, "invalid_body",
                       R"(expected {"kind": "proton"|"wine", "tag": "...", "source": "<optional source id>"})");
    }
    const std::string kind = body["kind"];
    const std::string tag = body["tag"];
    auto family = FamilyFor(config_, kind, body.value("source", std::string()));
    if (!family) return SendError(res, 404, family.error());

    auto releases = runner::ListFamilyReleases(*family);
    if (!releases) return SendError(res, 502, releases.error());
    const auto match = std::ranges::find(*releases, tag, &runner::ReleaseAsset::tag);
    if (match == releases->end()) {
      return SendError(res, 404, "release_not_found", std::format("no {} release tagged \"{}\"", family->label, tag));
    }
    InstallRunnerAsync(kind, family->id, *match, /*replacing=*/"");
    SendJson(res, {{"status", "downloading"}, {"tag", tag}, {"name", runner::ReleaseName(kind, *match)}}, 202);
  });

  // Only removable builds; the distro updates its own packages.
  http_->Get("/v1/runners/updates", [this](const Request&, Response& res) {
    const runner::RunnerRegistry registry(config_);
    json out = json::array();
    for (const auto& update : FindRunnerUpdates(config_, registry)) {
      out.push_back({{"reference", update.build.Reference()}, {"source", update.family.id},
                     {"tag", update.latest.tag}, {"name", runner::ReleaseName(update.build.kind, update.latest)},
                     {"label", runner::BuildLabel(update.build.kind,
                                                  runner::ReleaseName(update.build.kind, update.latest))}});
    }
    SendJson(res, std::move(out));
  });

  http_->Post("/v1/runners/update", [this](const Request& req, Response& res) {
    const json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("reference") || !body["reference"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"reference": "kind:name"})");
    }
    const std::string reference = body["reference"];
    const runner::RunnerRegistry registry(config_);
    for (const auto& update : FindRunnerUpdates(config_, registry)) {
      if (update.build.Reference() != reference) continue;
      InstallRunnerAsync(update.build.kind, update.family.id, update.latest, reference);
      return SendJson(res,
                      {{"status", "downloading"}, {"tag", update.latest.tag},
                       {"name", runner::ReleaseName(update.build.kind, update.latest)}},
                      202);
    }
    SendError(res, 409, "no_update", std::format("no newer release for \"{}\"", reference));
  });

  http_->Get("/v1/runners/tools", [](const Request&, Response& res) {
    const std::string umu = runner::UmuRunPath();
    const std::string winetricks = runner::WinetricksPath();
    SendJson(res, json::array({
                      {{"id", "umu"}, {"label", "umu-launcher"}, {"installed", !umu.empty()}, {"path", umu},
                       {"doc", "Runs Proton builds outside Steam. Without it no Proton build can be used."}},
                      {{"id", "winetricks"}, {"label", "winetricks"}, {"installed", !winetricks.empty()},
                       {"path", winetricks}, {"doc", "Installs runtimes and fixes into a game's prefix."}},
                  }));
  });

  http_->Post(R"(/v1/runners/tools/(umu|winetricks)/setup)", [this](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    std::optional<runner::ReleaseAsset> asset;
    if (id == "umu") {
      auto releases = runner::ListReleases(config_, "umu");
      if (!releases) return SendError(res, 502, releases.error());
      if (releases->empty()) return SendError(res, 404, "no_release_found", "no umu-launcher release found");
      asset = releases->front();
    }
    events_.Publish(id + ".setup.started", json::object());
    operations_.Run([this, id, asset] {
      Result<void> installed;
      if (asset) {
        auto path = runner::InstallToolBinary(config_, "umu", *asset, "umu-run");
        if (!path) installed = std::unexpected(path.error());
      } else {
        installed = runner::InstallWinetricks();
      }
      if (!installed) {
        log::Error("{} install failed: {}", id, installed.error().message);
        events_.Publish(id + ".setup.failed", FailedEvent(json::object(), installed.error()));
      } else {
        events_.Publish(id + ".setup.finished", json::object());
      }
    });
    SendJson(res, {{"status", "installing"}}, 202);
  });

  http_->Get(R"(/v1/runners/([^/]+)/schema)", [this](const Request& req, Response& res) {
    const runner::RunnerRegistry registry(config_);
    const runner::IRunner* found = registry.FindByKind(req.matches[1]);
    if (!found) return SendError(res, 404, "unknown_runner_kind", "no runner of that kind");
    SendJson(res, found->SettingsSchema());
  });

  // Only builds inside a search path can be removed, so the system wine can't be.
  http_->Delete(R"(/v1/runners/([^:]+):(.+))", [this](const Request& req, Response& res) {
    const std::string kind = req.matches[1];
    const std::string name = req.matches[2];
    if (name == "auto" || name == "latest") {
      return SendError(res, 400, "invalid_reference", "name a concrete build, not \"auto\"/\"latest\"");
    }

    const runner::RunnerRegistry registry(config_);
    auto resolved = registry.Resolve(kind + ":" + name);
    if (!resolved) return SendError(res, 404, resolved.error());
    if (!resolved->build) {
      return SendError(res, 400, "not_a_build", std::format("\"{}\" has no separate installed builds", kind));
    }

    if (auto deleted = DeleteUnderRoot(BuildDir(*resolved->build).string(), RunnerRoots(config_, kind)); !deleted) {
      return SendError(res, 400, deleted.error());
    }
    events_.Publish("runners.removed", {{"kind", kind}, {"name", name}});
    SendJson(res, json::object());
  });

  // --- events (SSE) -----------------------------------------------------

  http_->Get("/v1/events", [this](const Request& req, Response& res) {
    std::int64_t after_id = 0;
    bool resuming = false;
    if (auto it = req.headers.find("Last-Event-ID"); it != req.headers.end()) {
      after_id = std::atoll(it->second.c_str());
      resuming = true;
    }
    // A new client gets the buffer replayed, then `stream.live` so it can tell
    // history (show it) from news (announce it). A resuming one only missed news.
    const std::int64_t replay_end = resuming ? 0 : events_.LatestId();
    bool live_sent = resuming;

    res.set_header("Cache-Control", "no-cache");
    // The 20s wait only checks whether this client went away.
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, after_id, replay_end, live_sent](size_t, httplib::DataSink& sink) mutable -> bool {
          if (stopping_.load(std::memory_order_relaxed)) return false;
          if (!live_sent && after_id >= replay_end) {
            live_sent = true;
            static constexpr std::string_view kLive = "event: stream.live\ndata: {}\n\n";
            return sink.write(kLive.data(), kLive.size());
          }
          auto event = events_.WaitNext(after_id, stopping_, std::chrono::milliseconds(20000));
          if (!event) return sink.is_writable() && !stopping_.load(std::memory_order_relaxed);
          after_id = event->id;
          const std::string frame =
              std::format("id: {}\nevent: {}\ndata: {}\n\n", event->id, event->type,
                         event->payload.dump());
          return sink.write(frame.data(), frame.size());
        });
  });
}

void Server::StartJob(const Request& req, Response& res, const std::string& kind, const std::string& target,
                      const std::string& label, JobRegistry::Work work) {
  const std::string id = jobs_.Start(kind, target, label, std::move(work), req.get_param_value("job"));
  SendJson(res, {{"status", "running"}, {"job", id}}, 202);
}

Result<json> Server::RefreshMetadata(std::vector<model::Game> games, JobRegistry::Progress& progress) {
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
    metadata_fetches_.Enqueue(config_, events_, std::move(game), /*force=*/true, /*announce=*/false,
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
    if (stopping_.load(std::memory_order_relaxed)) {
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

Result<void> Server::DeleteGameData(const model::Game& game, bool files, bool prefix, bool metadata) {
  if (supervisor_.IsRunning(game.id)) return std::unexpected(GameRunningError(game.id));
  // A desktop-entry import only links to another app's own files.
  if (game.source == "desktop-entry") files = prefix = false;
  if (files && game.source == "epic" && !game.source_ref.empty()) {
    // Uninstall through Legendary so its manifest stays in sync.
    if (auto uninstalled = epic::RunLegendary(config_, {"uninstall", game.source_ref, "-y"}); !uninstalled) {
      return std::unexpected(uninstalled.error());
    }
  } else if (files) {
    if (auto deleted = DeleteUnderRoot(game.install_path, config_.GetPathArray("library_roots")); !deleted) {
      return deleted;
    }
  }
  if (prefix) {
    if (auto deleted = DeleteUnderRoot(game.data_dir, {config_.GetPath("prefix_root")}); !deleted) return deleted;
  }
  if (metadata) {
    // Metadata lives in Mira's own folder, keyed by id, so no root check is needed.
    std::error_code ec;
    std::filesystem::remove(metadata::MetadataFile(config_, game.id), ec);
    if (ec) log::Warn("could not remove metadata for {}: {}", game.id, ec.message());
    std::filesystem::remove_all(metadata::ArtworkDir(config_, game.id), ec);
    if (ec) log::Warn("could not remove artwork for {}: {}", game.id, ec.message());
  }
  return {};
}

void Server::InstallRunnerAsync(const std::string& kind, const std::string& source,
                                const runner::ReleaseAsset& asset, const std::string& replacing) {
  const std::string name = runner::ReleaseName(kind, asset);
  const json base = {{"kind", kind}, {"tag", asset.tag}, {"name", name},
                     {"label", runner::BuildLabel(kind, name)}, {"source", source}};
  events_.Publish("runners.download.started", base);
  operations_.Run([this, kind, asset, replacing, base] {
    if (auto installed = runner::DownloadAndInstall(config_, kind, asset); !installed) {
      log::Error("runner download failed ({} {}): {}", kind, asset.tag, installed.error().message);
      events_.Publish("runners.download.failed", FailedEvent(base, installed.error()));
      return;
    }
    log::Info("installed {} {}", kind, asset.tag);
    library::RetryBrokenProvisioning(config_, games_, events_);
    json finished = base;
    if (!replacing.empty()) {
      // Move what used the old build onto the new one.
      const runner::RunnerRegistry registry(config_);
      const std::vector<model::RunnerBuild> builds = BuildsOfKind(registry, kind);
      const auto fresh = std::ranges::find_if(builds, [&](const model::RunnerBuild& build) {
        return runner::IsInstalledAs(kind, build.name, BuildDir(build).filename().string(), asset);
      });
      if (fresh != builds.end()) {
        const std::string to = fresh->Reference();
        json moved = json::array();
        const auto batch = games_.BatchSaves();
        for (const model::Game& game : games_.All()) {
          if (game.runner_ref != replacing) continue;
          if (auto updated = games_.Update(game.id, [&](model::Game& g) { g.runner_ref = to; })) {
            moved.push_back(Record(*updated));
          }
        }
        if (config_.GetString("default_runner.windows") == replacing) (void)config_.Set("default_runner.windows", to);
        if (!moved.empty()) events_.Publish("games.updated", {{"games", moved}});
        events_.Publish("runners.updated",
                        {{"kind", kind}, {"from", replacing}, {"to", to}, {"games", moved.size()}});
        finished["replaced"] = replacing;
      }
    }
    events_.Publish("runners.download.finished", finished);
  });
}

}  // namespace mira::api
