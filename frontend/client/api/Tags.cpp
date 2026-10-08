#include "Tags.h"

#include <json.hpp>
#include <utility>

#include "../Async.h"
#include "../JsonMapping.h"
#include "../Transport.h"
#include "Request.h"

namespace mira_gui::api {
namespace {

using nlohmann::json;

std::vector<std::string> Strings(const json& entry, const char* key) {
  std::vector<std::string> out;
  for (const json& value : entry.value(key, json::array())) {
    if (value.is_string()) out.push_back(value.get<std::string>());
  }
  return out;
}

// The games a tag change touched, as POST /v1/tags/* reply with them.
PatchGamesResult GamesReply(const transport::Reply& reply, const std::string& endpoint) {
  return ReadReply<PatchGamesResult>(reply, endpoint, Shape::Object, [](PatchGamesResult& result, const json& body) {
    for (const json& entry : body.value("games", json::array())) result.games.push_back(mapping::ToGameSummary(entry));
  });
}

}  // namespace

void GetTagsAsync(QObject* context, std::function<void(TagsResult)> callback) {
  async::Run(
      context,
      [] {
        return ReadReply<TagsResult>(
            transport::Get("/v1/tags"), "GET /v1/tags", Shape::Object, [](TagsResult& result, const json& body) {
              for (const json& entry : body.value("tags", json::array())) {
                result.tags.push_back({entry.value("name", std::string()), Strings(entry, "ids"),
                                       entry.value("folder", false), Strings(entry, "steam_ids")});
              }
              for (const json& entry : body.value("steam", json::array())) {
                result.steam.push_back({entry.value("name", std::string()), Strings(entry, "ids")});
              }
              result.steam_missing = body.value("steam_missing", 0);
            });
      },
      std::move(callback));
}

void FetchSteamTagsAsync(QObject* context, std::function<void(SteamTagsFetchResult)> callback) {
  RunJob<SteamTagsFetchResult>(
      context, "tags", [](const std::string& query) { return transport::Post("/v1/tags/fetch" + query); },
      [](SteamTagsFetchResult& result, const json& body) { result.fetched = body.value("fetched", 0); },
      std::move(callback));
}

void SetTagAsync(QObject* context, const std::string& name, const std::vector<std::string>& ids,
                 std::optional<bool> folder, std::function<void(PatchGamesResult)> callback) {
  async::Run(
      context,
      [name, ids, folder] {
        json body = {{"name", name}, {"ids", ids}};
        if (folder) body["folder"] = *folder;
        return GamesReply(transport::PostJson("/v1/tags/set", body), "POST /v1/tags/set");
      },
      std::move(callback));
}

void RenameTagAsync(QObject* context, const std::string& from, const std::string& to,
                    std::function<void(PatchGamesResult)> callback) {
  async::Run(
      context,
      [from, to] {
        return GamesReply(transport::PostJson("/v1/tags/rename", {{"from", from}, {"to", to}}),
                          "POST /v1/tags/rename");
      },
      std::move(callback));
}

void RemoveTagAsync(QObject* context, const std::string& name, std::function<void(PatchGamesResult)> callback) {
  async::Run(
      context,
      [name] { return GamesReply(transport::PostJson("/v1/tags/remove", {{"name", name}}), "POST /v1/tags/remove"); },
      std::move(callback));
}

void PreviewTagsAsync(QObject* context, std::optional<std::vector<std::string>> folders,
                      std::optional<std::vector<std::string>> sorted_roots,
                      const std::map<std::string, std::vector<std::string>>& tags,
                      std::function<void(FolderTagsPreviewResult)> callback) {
  async::Run(
      context,
      [folders, sorted_roots, tags] {
        json body = json::object();
        if (folders) body["folders"] = *folders;
        if (sorted_roots) body["sorted_roots"] = *sorted_roots;
        if (!tags.empty()) body["tags"] = tags;
        return ReadReply<FolderTagsPreviewResult>(
            transport::PostJson("/v1/tags/preview", body), "POST /v1/tags/preview", Shape::Object,
            [](FolderTagsPreviewResult& result, const json& reply) {
              for (const json& move : reply.value("moving", json::array())) {
                result.moving.push_back({move.value("id", std::string()), move.value("name", std::string()),
                                         move.contains("to") && move["to"].is_string() ? move["to"].get<std::string>()
                                                                                       : std::string()});
              }
            });
      },
      std::move(callback));
}

}  // namespace mira_gui::api
