#include <httplib.h>

#include "api/Http.h"
#include "api/Routes.h"
#include "api/Services.h"
#include "saves/Backups.h"
#include "saves/Manifest.h"
#include "saves/Saves.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

constexpr int kSearchLimit = 50;

}  // namespace

void RegisterSaveRoutes(Router& http, Services& s) {
  // Answers from the index and the disk, never the network; fetches the list when it's due.
  http.Get(R"(/v1/games/([^/]+)/saves)", [&s](const Request& req, Response& res) {
    const auto game = s.games.Find(req.matches[1]);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    s.CheckSaveManifest();
    SendJson(res, saves::Overview(s.config, s.games.Metadata(), *game));
  });

  http.Post(R"(/v1/games/([^/]+)/saves/backup)", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    const auto game = s.games.Find(id);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    s.StartJob(
        req, res, "saves", id, std::format("Backing up {}'s saves", game->name),
        [&s, id](JobRegistry::Progress&) { return s.BackUpSaves(id, saves::kManual); }, &s.saves);
  });

  http.Post(R"(/v1/games/([^/]+)/saves/restore)", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"snapshot": "<id>"})";
    const std::string id = req.matches[1];
    const auto game = s.games.Find(id);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const std::string snapshot = body->value("snapshot", std::string());
    if (!saves::FindSnapshot(saves::GameBackups(s.config, id), snapshot))
      return SendError(res, 404, "snapshot_not_found", "no such backup");
    s.StartJob(
        req, res, "saves", id, std::format("Restoring {}'s saves", game->name),
        [&s, id, snapshot](JobRegistry::Progress&) { return s.RestoreSaves(id, snapshot); },
        &s.saves);
  });

  // Not a claim: a backup can go while the game runs.
  http.Delete(R"(/v1/games/([^/]+)/saves/([^/]+))", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    if (!s.games.Find(id)) return SendError(res, 404, "game_not_found", "no such game");
    if (auto deleted = saves::DeleteSnapshot(s.config, id, req.matches[2].str()); !deleted)
      return SendError(res, deleted.error().code == "snapshot_not_found" ? 404 : 500,
                       deleted.error());
    s.events.Publish("game.saves_changed", {{"id", id}});
    SendJson(res, json::object());
  });

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
