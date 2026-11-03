#include <httplib.h>
#include <signal.h>
#include <unistd.h>

#include "api/Http.h"
#include "api/Routes.h"
#include "api/Services.h"
#include "updates/Updates.h"

namespace mira::api {

void RegisterUpdateRoutes(Router& http, Services& s) {
  // Each update installs through its own route: the emulator's install, the store's setup, umu's
  // setup, /v1/runners/update, or for Mira /v1/updates/mira.
  http.Get("/v1/updates", [&s](const httplib::Request& req, httplib::Response& res) {
    nlohmann::json list = nlohmann::json::array();
    const bool refresh = req.get_param_value("refresh") == "1";
    for (const updates::Update& update : updates::FindUpdates(s.config, refresh)) {
      nlohmann::json entry = {{"kind", update.kind},     {"id", update.id},
                              {"name", update.name},     {"installed", update.installed},
                              {"latest", update.latest}, {"url", update.url}};
      if (!update.install.empty()) entry["install"] = update.install;
      list.push_back(std::move(entry));
    }
    SendJson(res, list);
  });

  http.Post("/v1/updates/mira", [&s](const httplib::Request& req, httplib::Response& res) {
    s.StartJob(req, res, "mira_update", "mira", "Updating Mira",
               [](JobRegistry::Progress& progress) -> Result<nlohmann::json> {
                 return updates::DownloadMira([&progress](int percent, const std::string& message) {
                   progress.Report(percent, 100, message);
                 });
               });
  });

  // For a client replacing mirad with a newer one. Games keep running and are adopted on restart.
  http.Post("/v1/shutdown", [](const httplib::Request&, httplib::Response& res) {
    SendJson(res, {{"status", "stopping"}}, 202);
    ::kill(::getpid(), SIGTERM);
  });
}

}  // namespace mira::api
