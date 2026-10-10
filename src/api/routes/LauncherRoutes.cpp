#include "api/Routes.h"

#include <format>

#include <httplib.h>

#include "api/Http.h"
#include "api/Services.h"
#include "core/Log.h"
#include "launchers/Launchers.h"
#include "launchers/Office.h"
#include "runner/Exec.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

// Adds Microsoft 365's apps, announcing the run as the launcher's own install so its progress shows.
Result<void> AddOfficeApps(Services& s, std::vector<std::string> apps) {
  const auto added = launchers::AddOfficeApps(s.config, s.games, s.events, std::move(apps));
  if (added) {
    s.SyncDesktopEntries();
  } else {
    log::Warn("adding Microsoft 365 apps failed: {}", added.error().message);
    s.events.Publish("launcher.install.failed", FailedEvent({{"id", "office"}}, added.error()));
  }
  return added;
}
}  // namespace

void RegisterLauncherRoutes(httplib::Server& http, Services& s) {
  http.Get("/v1/launchers", [&s](const Request&, Response& res) {
    json list = json::array();
    for (const launchers::Launcher& launcher : launchers::All()) {
      const auto game = s.games.Find(launchers::GameId(launcher));
      json entry = {{"id", launcher.id},
                    {"name", launcher.name},
                    {"kind", launcher.kind},
                    {"game_id", launchers::GameId(launcher)},
                    {"installed", launchers::Installed(s.games, launcher)},
                    {"install_state", launchers::InstallState(launcher)},
                    {"interactive_install", launcher.interactive},
                    {"packages", launcher.tricks.empty() ? "" : "winetricks"},
                    {"prefix", game ? game->data_dir : ""},
                    {"runner_ref", game ? game->runner_ref : ""},
                    {"error", game ? game->last_error : ""}};
      // What it can add, so they can be picked before it's installed.
      if (launcher.id == "office") {
        entry["apps"] = json::array();
        for (const launchers::office::App& app : launchers::office::Apps()) {
          entry["apps"].push_back({{"ref", app.ref}, {"name", app.name}});
        }
      }
      list.push_back(std::move(entry));
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
                 const auto done = launchers::Install(s.config, s.games, *launcher, [&s, launcher](double fraction) {
                   s.events.Publish("launcher.install.progress", {{"id", launcher->id}, {"progress", fraction}});
                 });
                 if (const auto stored = s.games.Find(launchers::GameId(*launcher))) {
                   s.events.Publish("game.updated", s.Record(*stored));
                 }
                 if (!done) {
                   if (launcher->id == "office") launchers::DropQueuedOfficeApps();
                   s.events.Publish("launcher.install.failed", FailedEvent({{"id", launcher->id}}, done.error()));
                   return std::unexpected(done.error());
                 }
                 if (const auto imported = launchers::Import(s.config, s.games, s.events, *launcher)) {
                   s.AfterImport(imported->added_games);
                 } else {
                   s.SyncDesktopEntries();
                 }
                 s.events.Publish("launcher.install.finished", {{"id", launcher->id}});
                 // Apps picked while the prefix was being set up. Their failure is announced on its own:
                 // the prefix itself is ready.
                 if (launcher->id == "office") (void)AddOfficeApps(s, {});
                 return json{{"id", launcher->id}};
               },
               &s.operations);
  });

  http.Post("/v1/launchers/office/apps", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"apps": ["word", "excel", ...]})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    if (!body->contains("apps") || !(*body)["apps"].is_array() || (*body)["apps"].empty()) {
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    }
    std::vector<std::string> apps;
    for (const json& app : (*body)["apps"]) {
      if (!app.is_string()) return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
      if (!std::ranges::contains(launchers::office::Apps(), app.get<std::string>(), &launchers::office::App::ref)) {
        return SendError(res, 400, "unknown_app",
                         std::format("Microsoft 365 has no app \"{}\"", app.get<std::string>()));
      }
      apps.push_back(app);
    }
    if (launchers::QueueOfficeApps(apps)) return SendJson(res, {{"status", "queued"}});
    const launchers::Launcher& office = *launchers::Find("office");
    if (!launchers::Installed(s.games, office)) {
      return SendError(res, 409, "launcher_not_installed", "Microsoft 365 isn't installed yet");
    }
    s.StartJob(req, res, "install", "office", "Adding Microsoft 365 apps",
               [&s, apps](JobRegistry::Progress&) -> Result<json> {
                 if (auto added = AddOfficeApps(s, apps); !added) return std::unexpected(added.error());
                 return json{{"apps", apps}};
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
