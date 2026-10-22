#include <httplib.h>

#include "api/Http.h"
#include "api/Routes.h"
#include "api/Services.h"
#include "saves/Manifest.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

constexpr int kSearchLimit = 50;

}  // namespace

void RegisterSaveRoutes(Router& http, Services& s) {
  http.Get("/v1/saves/manifest", [&s](const Request&, Response& res) {
    SendJson(res, saves::ManifestState(s.config, s.games.Metadata()));
  });

  http.Post("/v1/saves/manifest/update", [&s](const Request& req, Response& res) {
    SendJson(res, {{"status", "running"}, {"job", s.UpdateSaveManifest(Param(req, "job"))}}, 202);
  });

  // For picking a game's entry by hand. Fetches the list when it's due, answering from the one
  // there is meanwhile.
  http.Get("/v1/saves/entries", [&s](const Request& req, Response& res) {
    s.CheckSaveManifest();
    json entries = json::array();
    for (const auto& entry :
         s.games.Metadata().SearchSaveEntries(saves::Normalize(Param(req, "q")), kSearchLimit)) {
      json item = {{"name", entry.name}};
      if (!entry.alias_of.empty()) item["alias_of"] = entry.alias_of;
      entries.push_back(std::move(item));
    }
    SendJson(res, {{"entries", std::move(entries)}});
  });
}

}  // namespace mira::api
