#include "api/Routes.h"

#include <httplib.h>

#include "api/Http.h"
#include "api/Services.h"
#include "modmanagers/ModManagers.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

json ToJson(const Services& s, const modmanagers::ModManager& manager) {
  json on = json::array();
  for (const model::Game& game : s.games.All()) {
    if (modmanagers::OnGame(game, manager) && !s.Conceals(game)) on.push_back(game.id);
  }
  return {{"id", manager.id},
          {"name", manager.name},
          {"installed", modmanagers::Installed(manager)},
          {"version", modmanagers::InstalledVersion(manager)},
          {"games", std::move(on)}};
}

// The mod manager the path names, or null after a 404.
const modmanagers::ModManager* FindManager(const std::string& id, Response& res) {
  const modmanagers::ModManager* manager = modmanagers::Find(id);
  if (!manager) SendError(res, 404, "mod_manager_not_found", "no such mod manager");
  return manager;
}

void Changed(Services& s, const Result<model::Game>& game) {
  if (game) s.events.Publish("game.updated", s.Record(*game));
}

}  // namespace

void RegisterModManagerRoutes(Router& http, Services& s) {
  http.Get("/v1/mod-managers", [&s](const Request&, Response& res) {
    json list = json::array();
    for (const modmanagers::ModManager& manager : modmanagers::All()) list.push_back(ToJson(s, manager));
    SendJson(res, list);
  });

  http.Post(R"(/v1/mod-managers/([^/]+)/install)", [&s](const Request& req, Response& res) {
    const modmanagers::ModManager* manager = FindManager(req.matches[1], res);
    if (!manager) return;
    s.StartJob(req, res, "install", manager->id, "Installing " + manager->name,
               [&s, manager](JobRegistry::Progress&) -> Result<json> {
                 if (auto installed = modmanagers::Install(*manager); !installed) return std::unexpected(installed.error());
                 s.games.MarkSourceAdded(manager->id, false);
                 s.events.Publish("sources.changed", json::object());
                 return ToJson(s, *manager);
               });
  });

  // Adds it to the game, readying the game's prefix first, which can take minutes.
  http.Post(R"(/v1/games/([^/]+)/mod-managers/([^/]+))", [&s](const Request& req, Response& res) {
    const std::string id = req.matches[1];
    const modmanagers::ModManager* manager = FindManager(req.matches[2], res);
    if (!manager) return;
    const auto game = s.games.Find(id);
    if (!game) return SendError(res, 404, "game_not_found", "no such game");
    if (!modmanagers::CanAdd(*game)) {
      return SendError(res, 409, "not_windows_game", manager->name + " runs in a Windows game's prefix, and this game has none");
    }
    s.StartJob(req, res, "mod_manager", id, "Adding " + manager->name + " to " + game->name,
               [&s, id, manager](JobRegistry::Progress&) -> Result<json> {
                 const auto added = modmanagers::AddToGame(s.config, s.games, *manager, id);
                 if (!added) return std::unexpected(added.error());
                 Changed(s, added);
                 return s.Record(*added);
               },
               &s.tricks);
  });

  http.Delete(R"(/v1/games/([^/]+)/mod-managers/([^/]+))", [&s](const Request& req, Response& res) {
    const modmanagers::ModManager* manager = FindManager(req.matches[2], res);
    if (!manager) return;
    const auto removed = modmanagers::RemoveFromGame(s.games, *manager, req.matches[1]);
    if (!removed) return SendError(res, 404, removed.error());
    Changed(s, removed);
    SendJson(res, s.Record(*removed));
  });
}

}  // namespace mira::api
