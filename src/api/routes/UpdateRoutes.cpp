#include "api/Routes.h"

#include <httplib.h>

#include "api/Http.h"
#include "api/Services.h"
#include "updates/Updates.h"

namespace mira::api {

void RegisterUpdateRoutes(httplib::Server& http, Services& s) {
  // Each update installs through its own route: the emulator's install, the store's setup, umu's setup,
  // or /v1/runners/update. Mira's own is a link to its release.
  http.Get("/v1/updates", [&s](const httplib::Request&, httplib::Response& res) {
    nlohmann::json list = nlohmann::json::array();
    for (const updates::Update& update : updates::FindUpdates(s.config)) {
      list.push_back({{"kind", update.kind}, {"id", update.id}, {"name", update.name},
                      {"installed", update.installed}, {"latest", update.latest}, {"url", update.url}});
    }
    SendJson(res, list);
  });
}

}  // namespace mira::api
