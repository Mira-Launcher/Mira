#include "api/Routes.h"

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>

#include <httplib.h>

#include "api/Http.h"
#include "api/Services.h"
#include "config/Resolver.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Result.h"
#include "core/Strings.h"
#include "launchers/Launchers.h"
#include "library/AutoInstall.h"
#include "library/Detector.h"
#include "library/Relocate.h"
#include "library/Scanner.h"
#include "runner/Exec.h"
#include "runner/RunnerRegistry.h"
#include "runner/Winetricks.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

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


struct RunProgram {
  std::string exe_path;
  std::string args;
};

// The command that starts `game` (or `program` in its prefix) under its resolved runner, with launch.env and
// command wrappers applied. Pins the runner on the game when launch.pin_runner is on and none was set.
Result<Command> PrepareCommand(Services& s, model::Game& game, const std::optional<RunProgram>& program) {
  const runner::RunnerRegistry registry(s.config);
  auto resolved = registry.Resolve(registry.ResolveRef(game));
  if (!resolved) return std::unexpected(resolved.error());

  if (game.runner_ref.empty() && resolved->build && s.config.GetBool("launch.pin_runner")) {
    const std::string pinned = std::format("{}:{}", resolved->runner->kind(), resolved->build->name);
    game.runner_ref = pinned;
    [[maybe_unused]] auto _ = s.games.Update(game.id, [&](model::Game& g) { g.runner_ref = pinned; });
  }

  model::Game target = game;
  if (program) {
    target.exe_path = program->exe_path;
    target.args = program->args;
  }
  auto command = resolved->runner->BuildCommand(target, resolved->build);
  if (!command) return std::unexpected(command.error());
  // Otherwise the spawned child's chdir fails and it exits 127 before anything is logged.
  if (std::error_code ec; !command->cwd.empty() && !std::filesystem::is_directory(command->cwd, ec)) {
    return Err("working_dir_missing",
               std::format("the folder the game starts in, \"{}\", doesn't exist", command->cwd.string()),
               "Check the game's folder is still there, or choose its executable again.",
               Fix::Game(game.id, "exe"));
  }

  const config::Resolver resolver(s.config, game.overrides);
  const std::vector<std::string> wrappers = resolver.GetStringArray("command_wrappers");
  if (auto checked = CheckCommandWrappers(wrappers); !checked) return std::unexpected(checked.error());
  ApplyLaunchEnv(*command, resolver.GetStringArray("launch.env"));
  ApplyCommandWrappers(*command, wrappers);
  return command;
}

}  // namespace

void RegisterLaunchRoutes(httplib::Server& http, Services& s) {
  // --- launching ------------------------------------------------------------

  http.Post(R"(/v1/games/([^/]+)/launch)", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    if (game->status == model::GameStatus::Broken &&
        library::RetryBrokenProvisioning(s.config, s.games, s.events, game->id)) {
      game = s.games.Find(game->id);
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

    const auto reservation = s.supervisor.Reserve(game->id);
    if (!reservation) {
      return SendError(res, 409, "already_running", std::format("\"{}\" is already running", game->id));
    }

    const config::Resolver resolver(s.config, game->overrides);
    const std::string pre_script = resolver.GetString("launch.pre_script");
    const std::string post_script = resolver.GetString("launch.post_script");

    // A launcher game is started by its launcher, which keeps running after the
    // game exits; the game's own processes are tracked.
    if (launchers::ForGame(*game)) {
      auto command = launchers::BuildCommand(s.config, s.games, *game);
      if (!command) return SendError(res, 409, command.error());
      if (auto ran = RunPreScriptInline(pre_script); !ran) {
        return SendError(res, 409, ran.error());
      }
      ApplyLaunchEnv(*command, resolver.GetStringArray("launch.env"));
      if (auto spawned = runner::SpawnDetached(*command); !spawned) {
        return SendError(res, 500, spawned.error());
      }
      [[maybe_unused]] auto _ =
          s.games.Update(game->id, [](model::Game& g) { g.last_played_at = model::NowSeconds(); });
      s.events.Publish("game.launched", {{"id", game->id}, {"via", "launcher"}, {"tracked", true}});
      if (auto started = s.supervisor.TrackLauncherLaunch(*game, launchers::WindowsDir(*game),
                                                         s.config.GetInt("launchers.detect_timeout_s"), post_script);
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
            s.games.Update(game->id, [](model::Game& g) { g.last_played_at = model::NowSeconds(); });
        const bool track = resolver.GetBool("steam.track_process");
        s.events.Publish("game.launched",
                        {{"id", game->id}, {"via", "steam"}, {"tracked", track}});
        if (track) {
          if (auto started = s.supervisor.TrackSteamLaunch(*game, appid, post_script); !started) {
            log::Warn("couldn't start tracking {}: {}", game->id, started.error().message);
          }
        }
        return SendJson(res, {{"status", "launched_via_steam"}, {"tracked", track}});
      }
    }

    auto command = PrepareCommand(s, *game, std::nullopt);
    if (!command) return SendError(res, command.error().code == "working_dir_missing" ? 409 : 400, command.error());

    // A Windows "game" that turns out to be an installer is caught at exit (CheckForInstall).
    // Only for an existing prefix: a new one's own Program Files would all look installed.
    if (std::error_code ec; game->platform == model::Platform::Windows && !game->data_dir.empty() &&
                            std::filesystem::exists(std::filesystem::path(game->data_dir) / "drive_c", ec)) {
      s.WatchForInstall(game->id, library::InstallFolders(s.config, game->data_dir));
    }

    // mira-run owns the session so it survives mirad dying. Without it the game is
    // launched directly, with no session record.
    const auto mira_run = runner::ResolveSiblingBinary(OwnBinaryDir(), "mira-run");
    if (!mira_run) {
      log::Warn("mira-run not found; launching {} directly with no session recording", game->id);
      if (auto ran = RunPreScriptInline(pre_script); !ran) {
        return SendError(res, 409, ran.error());
      }
      if (auto launched = s.supervisor.Launch(*game, *command, post_script); !launched) {
        return SendError(res, 409, launched.error());
      }
      return SendJson(res, {{"status", "running"}, {"tracked", true}});
    }

    const int pre_timeout_s = static_cast<int>(resolver.GetInt("launch.pre_timeout_s"));
    const std::filesystem::path sessions_dir = s.games.Dir() / "sessions";
    const std::filesystem::path log_file = s.games.Dir() / "logs" / std::format("{}.log", game->id);
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

    if (auto launched = s.supervisor.LaunchWrapped(*game, *wrapper_pid, std::filesystem::path(status.detail));
        !launched) {
      return SendError(res, 409, launched.error());
    }
    SendJson(res, {{"status", "running"}, {"tracked", true}});  // always true: Mira spawned it
  });

  http.Post(R"(/v1/games/([^/]+)/stop)", [&s](const Request& req, Response& res) {
    if (auto stopped = s.supervisor.Stop(req.matches[1]); !stopped) {
      // A client that missed the exit gets told it's stopped instead of an error.
      if (stopped.error().code == "not_running") {
        if (const auto game = s.games.Find(req.matches[1])) {
          json event = s.Record(*game);
          event["state"] = "idle";
          s.events.Publish("game.state", std::move(event));
          return SendJson(res, {{"status", "not_running"}});
        }
      }
      return SendError(res, 409, stopped.error());
    }
    SendJson(res, {{"status", "stopping"}});
  });

  http.Post(R"(/v1/games/([^/]+)/run)", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
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
      const runner::RunnerRegistry provisioner(s.config);
      const model::Game provisioned = provisioner.ProvisionGame(*game);
      auto saved = s.games.Update(game->id, [&](model::Game& g) {
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

    auto command = PrepareCommand(s, *game, RunProgram{exe_path, args});
    if (!command) return SendError(res, command.error().code == "working_dir_missing" ? 409 : 400, command.error());

    if (auto launched = s.supervisor.Launch(*game, *command); !launched) {
      return SendError(res, 409, launched.error());
    }
    SendJson(res, {{"status", "running"}});
  });

  // Describes a game's installer, or the file at ?path=.
  http.Get(R"(/v1/games/([^/]+)/installer)", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    if (req.has_param("path")) game->exe_path = req.get_param_value("path");
    const auto info = library::DescribeInstaller(s.config, *game);
    if (!info) return SendError(res, 404, info.error());
    SendJson(res, {{"path", info->path.string()},
                   {"size_bytes", info->size_bytes},
                   {"format", library::ToString(info->format)},
                   {"silent", info->silent},
                   {"silent_args", info->silent_args}});
  });

  http.Get(R"(/v1/games/([^/]+)/install/progress)", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!s.games.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
    const auto progress = library::Progress(id);
    if (!progress) return SendJson(res, {{"state", "idle"}});
    SendJson(res, {{"state", progress->state},
                   {"mode", progress->mode},
                   {"started_at", progress->started_at},
                   {"finished_at", progress->finished_at},
                   {"error", progress->error},
                   {"bytes_written", progress->bytes_written}});
  });

  http.Post(R"(/v1/games/([^/]+)/install)", [&s](const Request& req, Response& res) {
    const auto game = s.games.Find(req.matches[1]);
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

    s.StartJob(req, res, "install", game->id, "Installing " + game->name,
               [&s, id = game->id, interactive, installer](JobRegistry::Progress&) -> Result<json> {
                 if (auto done = library::RunInstall(
                         s.config, s.games, s.events, &s.fetches, id,
                         interactive ? library::InstallMode::kInteractive : library::InstallMode::kAuto, installer);
                     !done) {
                   return std::unexpected(done.error());
                 }
                 return json{{"id", id}};
               },
               &s.operations);
  });

  http.Post(R"(/v1/games/([^/]+)/finish-install)", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
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
        if (s.supervisor.IsRunning(game->id)) return SendError(res, 409, GameRunningError(game->id));
        const library::Detector::Result detected =
            library::Detector(library::SettingsFromConfig(s.config)).Detect(install_path);
        library::AdoptInstallFolder(*game, install_path);
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
    const bool moved = s.games.Find(game->id).value_or(*game).install_path != game->install_path;
    auto result = s.games.Update(game->id, [&](model::Game& g) {
      g.install_path = game->install_path;
      g.installer_dir = game->installer_dir;
      g.name = game->name;
      g.working_dir = game->working_dir;
      g.exe_path = game->exe_path;
      g.candidates = game->candidates;
      g.confidence = game->confidence;
      g.status = model::GameStatus::Ready;
      g.last_error.clear();
    });
    if (!result) return SendStoreError(res, result.error());
    s.SyncDesktopEntry(result->id);
    s.events.Publish("game.updated", s.Record(*result));
    // Its art and store info were looked up by the installer's name.
    if (moved) {
      s.fetches.Enqueue(s.config, s.events, *result);
      library::AnnounceInstallerLeftover(s.events, *result);
    }
    SendJson(res, s.Record(*result));
  });

  http.Delete(R"(/v1/games/([^/]+)/installer)", [&s](const Request& req, Response& res) {
    const auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    if (s.supervisor.IsRunning(game->id)) return SendError(res, 409, GameRunningError(game->id));
    if (auto deleted = library::DeleteInstallerFolder(s.config, *game); !deleted) {
      return SendError(res, deleted.error().code == "no_installer_dir" ? 404 : 409, deleted.error());
    }
    auto result = s.games.Update(game->id, [](model::Game& g) { g.installer_dir.clear(); });
    if (!result) return SendStoreError(res, result.error());
    s.events.Publish("game.updated", s.Record(*result));
    SendJson(res, s.Record(*result));
  });

  http.Post(R"(/v1/games/([^/]+)/relocate)", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
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

    if (s.supervisor.IsRunning(game->id)) return SendError(res, 409, GameRunningError(game->id));

    s.StartJob(req, res, "relocate", game->id, "Moving " + game->name,
             [&s, game = *game, request](JobRegistry::Progress&) -> Result<json> {
               auto folders_lock = s.games.LockFolders();
               auto relocated = library::Relocate(s.config, game, request);
               if (!relocated) return std::unexpected(relocated.error());
               auto saved = s.games.Update(game.id, [&](model::Game& g) {
                 g.install_path = relocated->install_path;
                 g.data_dir = relocated->data_dir;
                 g.updated_at = model::NowSeconds();
               });
               folders_lock.unlock();
               if (!saved) return std::unexpected(saved.error());
               s.SyncDesktopEntry(saved->id);
               json record = s.Record(*saved);
               s.events.Publish("game.updated", record);
               return record;
             });
  });

  http.Post(R"(/v1/games/([^/]+)/tricks)", [&s](const Request& req, Response& res) {
    auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");

    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded() || !body.contains("verb") || !body["verb"].is_string()) {
      return SendError(res, 400, "invalid_body", R"(expected {"verb": "..."})");
    }
    const std::string verb = body["verb"];
    const std::string id = game->id;

    s.events.Publish("tricks.started", {{"id", id}, {"verb", verb}});
    s.StartJob(req, res, "tricks", id, "Running winetricks " + verb,
               [&s, id, verb](JobRegistry::Progress&) -> Result<json> {
                 const runner::RunnerRegistry registry(s.config);
                 const auto game = s.games.Find(id);
                 if (!game) {  // removed while queued
                   const Error removed{"game_not_found", "the game was removed", {}, {}};
                   s.events.Publish("tricks.failed", FailedEvent({{"id", id}, {"verb", verb}}, removed));
                   return std::unexpected(removed);
                 }
                 if (auto ran = runner::RunTricksVerb(registry, *game, verb); !ran) {
                   log::Error("winetricks {} failed for {}: {}", verb, id, ran.error().message);
                   s.events.Publish("tricks.failed", FailedEvent({{"id", id}, {"verb", verb}}, ran.error()));
                   return std::unexpected(ran.error());
                 }
                 s.events.Publish("tricks.finished", {{"id", id}, {"verb", verb}});
                 return json{{"id", id}, {"verb", verb}};
               },
               &s.tricks);
  });
}

}  // namespace mira::api
