#include "ModManagers.h"

#include <json.hpp>

#include "../Async.h"
#include "../Transport.h"
#include "Request.h"

namespace mira_gui::api {
using nlohmann::json;
namespace {

std::string GamePath(const std::string& id, const std::string& game_id) {
  return "/v1/games/" + PercentEncode(game_id) + "/mod-managers/" + PercentEncode(id);
}

}  // namespace

void GetModManagersAsync(QObject* context, std::function<void(ModManagersResult)> callback) {
  async::Run(
      context,
      [] {
        return ReadReply<ModManagersResult>(
            transport::Get("/v1/mod-managers"), "GET /v1/mod-managers", Shape::Array,
            [](ModManagersResult& result, const json& body) {
              for (const json& entry : body) {
                result.managers.push_back(
                    {.id = entry.value("id", std::string()),
                     .name = entry.value("name", std::string()),
                     .installed = entry.value("installed", false),
                     .version = entry.value("version", std::string()),
                     .games = entry.value("games", std::vector<std::string>())});
              }
            });
      },
      std::move(callback));
}

void InstallModManagerAsync(QObject* context, const std::string& id,
                            std::function<void(StoreActionResult)> callback) {
  RunJob<StoreActionResult>(
      context, "install",
      [id](const std::string& query) {
        return transport::Post("/v1/mod-managers/" + PercentEncode(id) + "/install" + query);
      },
      [](StoreActionResult&, const json&) {}, std::move(callback));
}

void AddModManagerToGameAsync(QObject* context, const std::string& id, const std::string& game_id,
                              std::function<void(StoreActionResult)> callback) {
  RunJob<StoreActionResult>(
      context, "mod_manager",
      [id, game_id](const std::string& query) {
        return transport::Post(GamePath(id, game_id) + query);
      },
      [](StoreActionResult&, const json&) {}, std::move(callback));
}

void GetModManagerPurgeAsync(QObject* context, const std::string& id, const std::string& game_id,
                             std::function<void(ModManagerPurgeResult)> callback) {
  async::Run(
      context,
      [id, game_id] {
        return ReadReply<ModManagerPurgeResult>(
            transport::Get(GamePath(id, game_id)), "GET /v1/games/{id}/mod-managers/{manager}",
            Shape::Object, [](ModManagerPurgeResult& result, const json& body) {
              result.deployed = body.value("deployed", 0);
              result.restored = body.value("restored", 0);
              for (const json& folder : body.value("deleted", json::array()))
                result.deleted.push_back(folder.value("path", std::string()));
              result.bytes = body.value("bytes", std::uint64_t{0});
              result.open = body.value("open", false);
            });
      },
      std::move(callback));
}

void RemoveModManagerFromGameAsync(QObject* context, const std::string& id,
                                   const std::string& game_id,
                                   std::function<void(StoreActionResult)> callback) {
  RunJob<StoreActionResult>(
      context, "mod_manager",
      [id, game_id](const std::string& query) {
        return transport::Delete(GamePath(id, game_id) + query);
      },
      [](StoreActionResult&, const json&) {}, std::move(callback));
}

}  // namespace mira_gui::api
