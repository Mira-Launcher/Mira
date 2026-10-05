#include "api/Routes.h"

#include <format>

#include <httplib.h>

#include "api/Http.h"
#include "api/Services.h"
#include "launchers/Launchers.h"
#include "runner/Exec.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;
}  // namespace

void RegisterLauncherRoutes(httplib::Server& http, Services& s) {
  http.Get("/v1/launchers", [&s](const Request&, Response& res) {
    json list = json::array();
    for (const launchers::Launcher& launcher : launchers::All()) {
      const auto game = s.games.Find(launchers::GameId(launcher));
      list.push_back({{"id", launcher.id},
                      {"name", launcher.name},
                      {"game_id", launchers::GameId(launcher)},
                      {"installed", launchers::Installed(s.games, launcher)},
                      {"install_state", launchers::InstallState(launcher)},
                      {"interactive_install", launcher.interactive},
                      {"prefix", game ? game->data_dir : ""},
                      {"runner_ref", game ? game->runner_ref : ""},
                      {"error", game ? game->last_error : ""}});
    }
    SendJson(res, list);
  });

  http.Post(R"(/v1/launchers/([^/]+)/install)", [&s](const Request& req, Response& res) {
    const launchers::Launcher* launcher = launchers::Find(req.matches[1].str());
    if (!launcher) return SendError(res, 404, "launcher_not_found", "no such launcher");
    if (!launchers::BeginInstall(*launcher)) {
      return SendError(res, 409, "install_running", std::format("{} is already installing", launcher->name));
    }
    s.events.Publish("launcher.install.started", {{"id", launcher->id}});
    s.StartJob(req, res, "install", launcher->id, "Installing " + launcher->name,
               [&s, launcher](JobRegistry::Progress&) -> Result<json> {
                 const auto done = launchers::Install(s.config, s.games, *launcher);
                 if (const auto stored = s.games.Find(launchers::GameId(*launcher))) {
                   s.events.Publish("game.updated", s.Record(*stored));
                 }
                 if (!done) {
                   s.events.Publish("launcher.install.failed", FailedEvent({{"id", launcher->id}}, done.error()));
                   return std::unexpected(done.error());
                 }
                 if (const auto imported = launchers::Import(s.config, s.games, s.events, *launcher)) {
                   s.AfterImport(imported->added_games);
                 } else {
                   s.SyncDesktopEntries();
                 }
                 s.events.Publish("launcher.install.finished", {{"id", launcher->id}});
                 return json{{"id", launcher->id}};
               },
               &s.operations);
  });

  http.Post(R"(/v1/launchers/([^/]+)/import)", [&s](const Request& req, Response& res) {
    const launchers::Launcher* launcher = launchers::Find(req.matches[1].str());
    if (!launcher) return SendError(res, 404, "launcher_not_found", "no such launcher");
    s.StartJob(req, res, "import", launcher->id, "Importing from " + launcher->name,
             [&s, launcher](JobRegistry::Progress&) -> Result<json> {
               const auto summary = launchers::Import(s.config, s.games, s.events, *launcher);
               if (!summary) return std::unexpected(summary.error());
               s.AfterImport(summary->added_games);
               return json{{"added", summary->added}, {"updated", summary->updated}};
             });
  });

  http.Post(R"(/v1/launchers/([^/]+)/open)", [&s](const Request& req, Response& res) {
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
    auto command = launchers::BuildCommand(s.config, s.games, target, action);
    if (!command) return SendError(res, 409, command.error());
    if (auto spawned = runner::SpawnDetached(*command); !spawned) {
      return SendError(res, 500, spawned.error());
    }
    SendJson(res, {{"status", "opened"}});
  });
}

}  // namespace mira::api
