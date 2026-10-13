#include "ModManagers.h"

#include <json.hpp>

#include "../Async.h"
#include "../Transport.h"
#include "Request.h"

namespace mira_gui::api {
using nlohmann::json;

void GetModManagersAsync(QObject* context, std::function<void(ModManagersResult)> callback) {
  async::Run(
      context,
      [] {
        return ReadReply<ModManagersResult>(
            transport::Get("/v1/mod-managers"), "GET /v1/mod-managers", Shape::Array,
            [](ModManagersResult& result, const json& body) {
              for (const json& entry : body) {
                result.managers.push_back({.id = entry.value("id", std::string()),
                                           .name = entry.value("name", std::string()),
                                           .installed = entry.value("installed", false),
                                           .version = entry.value("version", std::string()),
                                           .games = entry.value("games", std::vector<std::string>())});
              }
            });
      },
      std::move(callback));
}

void InstallModManagerAsync(QObject* context, const std::string& id, std::function<void(StoreActionResult)> callback) {
  RunJob<StoreActionResult>(
      context, "install",
      [id](const std::string& query) { return transport::Post("/v1/mod-managers/" + PercentEncode(id) + "/install" + query); },
      [](StoreActionResult&, const json&) {}, std::move(callback));
}

void AddModManagerToGameAsync(QObject* context, const std::string& id, const std::string& game_id,
                              std::function<void(StoreActionResult)> callback) {
  RunJob<StoreActionResult>(
      context, "mod_manager",
      [id, game_id](const std::string& query) {
        return transport::Post("/v1/games/" + PercentEncode(game_id) + "/mod-managers/" + PercentEncode(id) + query);
      },
      [](StoreActionResult&, const json&) {}, std::move(callback));
}

}  // namespace mira_gui::api
