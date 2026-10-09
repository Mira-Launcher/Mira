#include "Stores.h"

#include <cctype>
#include <chrono>
#include <json.hpp>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "../Async.h"
#include "../Jobs.h"
#include "../JsonMapping.h"
#include "../Transport.h"
#include "Request.h"

namespace mira_gui::api {
namespace {

using nlohmann::json;

std::string StorePath(const std::string& source, const std::string& action) {
  return "/v1/stores/" + source + "/" + action;
}

StoreStatusResult GetStoreStatusSync(const std::string& source) {
  const std::string path = StorePath(source, "status");
  // Humble's status asks humble-cli itself, over the network.
  return ReadReply<StoreStatusResult>(
      transport::Get(path, {.read_timeout = std::chrono::seconds(30)}), "GET " + path,
      Shape::Object, [](StoreStatusResult& result, const json& body) {
        const json tool = body.value("tool", json::object());
        if (tool.is_object()) {
          result.tool_installed = tool.value("installed", false);
          result.tool_version = tool.value("version", std::string());
        }
        result.authenticated = body.value("authenticated", false);
        result.account = body.value("account", std::string());
      });
}

StoreActionResult SignInStoreSync(const std::string& source, const std::string& credential) {
  const transport::Reply reply =
      transport::PostJson(StorePath(source, "login"), {{"credential", credential}},
                          {.read_timeout = std::chrono::seconds(60)});
  return {reply.ok, reply.error};
}

StoreActionResult SignOutStoreSync(const std::string& source) {
  const transport::Reply reply = transport::Post(StorePath(source, "logout"));
  return {reply.ok, reply.error};
}

StoreLibraryResult GetStoreLibrarySync(const std::string& source, bool fresh) {
  // Every store at once asks each in turn, so it gets longer.
  std::string path = source.empty() ? "/v1/library" : "/v1/library?source=" + PercentEncode(source);
  if (fresh) path += source.empty() ? "?fresh=1" : "&fresh=1";
  return ReadReply<StoreLibraryResult>(
      transport::Get(path, {.read_timeout = std::chrono::seconds(source.empty() ? 180 : 60)}),
      "GET /v1/library", Shape::Array, [source](StoreLibraryResult& result, const json& body) {
        for (const json& entry : body) {
          if (!entry.is_object()) continue;
          std::vector<std::string> steam_tags;
          for (const json& tag : entry.value("steam_tags", json::array())) {
            if (tag.is_string()) steam_tags.push_back(tag.get<std::string>());
          }
          const json reviews = entry.value("steam_reviews", json::object());
          result.titles.push_back({.ref = entry.value("ref", std::string()),
                                   .title = entry.value("title", std::string()),
                                   .installed = entry.value("installed", false),
                                   .owned = entry.value("owned", true),
                                   .source = entry.value("source", source),
                                   .protondb_tier = entry.value("protondb_tier", std::string()),
                                   .steam_tags = std::move(steam_tags),
                                   .review_summary = reviews.value("score_description", std::string()),
                                   .review_percent = reviews.value("percent_positive", -1)});
        }
      });
}

StoreActionResult InstallStoreTitleSync(const std::string& source, const std::string& ref,
                                        bool update) {
  const transport::Reply reply = transport::PostJson(
      update ? "/v1/library/update" : "/v1/library/install", {{"source", source}, {"ref", ref}});
  return {reply.ok, reply.error};
}

StoreActionResult PauseStoreInstallSync(const std::string& source, const std::string& ref) {
  const transport::Reply reply =
      transport::PostJson("/v1/library/install/pause", {{"source", source}, {"ref", ref}});
  return {reply.ok, reply.error};
}

StoreActionResult DiscardPausedInstallSync(const std::string& source, const std::string& ref) {
  const transport::Reply reply = transport::Delete("/v1/library/install/paused?source=" +
                                                   PercentEncode(source) + "&ref=" + PercentEncode(ref));
  return {reply.ok, reply.error};
}

PausedInstallsResult ListPausedInstallsSync() {
  return ReadReply<PausedInstallsResult>(
      transport::Get("/v1/library/install/paused"), "GET /v1/library/install/paused", Shape::Array,
      [](PausedInstallsResult& result, const json& body) {
        for (const json& entry : body) {
          if (!entry.is_object()) continue;
          result.installs.push_back({.source = entry.value("source", std::string()),
                                     .ref = entry.value("ref", std::string()),
                                     .update = entry.value("update", false)});
        }
      });
}

HumbleLibraryResult GetHumbleLibrarySync() {
  return ReadReply<HumbleLibraryResult>(
      transport::Get("/v1/stores/humble/bundles", {.read_timeout = std::chrono::seconds(60)}),
      "GET /v1/stores/humble/bundles", Shape::Array,
      [](HumbleLibraryResult& result, const json& body) {
        for (const json& entry : body) {
          if (!entry.is_object()) continue;
          result.bundles.push_back({.key = entry.value("key", std::string()),
                                    .name = entry.value("name", std::string()),
                                    .claimed = entry.value("claimed", false)});
        }
      });
}

StoreActionResult QueueTitleArtworkSync(const std::string& source,
                                        const std::vector<StoreTitle>& titles) {
  json list = json::array();
  for (const StoreTitle& title : titles)
    list.push_back({{"ref", title.ref}, {"title", title.title}});
  const transport::Reply reply =
      transport::PostJson("/v1/library/artwork", {{"source", source}, {"titles", list}});
  return {reply.ok, reply.error};
}

RemovalPlanResult GetRemovalPlanSync(const std::string& source) {
  return ReadReply<RemovalPlanResult>(
      transport::Get("/v1/sources/" + PercentEncode(source) + "/removal"),
      "GET /v1/sources/" + source + "/removal", Shape::Object,
      [](RemovalPlanResult& result, const json& body) {
        for (const json& game : body.value("games", json::array())) {
          result.games.push_back({.id = game.value("id", std::string()),
                                  .name = game.value("name", std::string()),
                                  .deletes = game.value("deletes", std::string())});
        }
        result.launcher_dir = body.value("launcher_dir", std::string());
        for (const json& path : body.value("kept", json::array())) {
          if (path.is_string()) result.kept.push_back(path.get<std::string>());
        }
        result.signs_out = body.value("signs_out", false);
      });
}

void FillRemoveSource(RemoveSourceResult& result, const json& body) {
  result.removed = body.value("removed", 0);
  for (const json& problem : body.value("problems", json::array())) {
    if (problem.is_string()) result.problems.push_back(problem.get<std::string>());
  }
}

SourceRunnerResult SourceRunnerFrom(const transport::Reply& reply, const std::string& endpoint) {
  return ReadReply<SourceRunnerResult>(
      reply, endpoint, Shape::Object, [](SourceRunnerResult& result, const json& body) {
        result.runner_ref = body.value("runner_ref", std::string());
        result.games = body.value("games", 0);
        result.differing = body.value("differing", 0);
      });
}

SourceRunnerResult GetSourceRunnerSync(const std::string& source) {
  return SourceRunnerFrom(transport::Get("/v1/sources/" + PercentEncode(source) + "/runner"),
                          "GET /v1/sources/" + source + "/runner");
}

SourceRunnerResult SetSourceRunnerSync(const std::string& source, const std::string& runner_ref,
                                       bool apply_to_games) {
  return SourceRunnerFrom(
      transport::PostJson("/v1/sources/" + PercentEncode(source) + "/runner",
                          {{"runner_ref", runner_ref}, {"apply_to_games", apply_to_games}}),
      "POST /v1/sources/" + source + "/runner");
}

ItchCollectionsResult GetItchCollectionsSync() {
  return ReadReply<ItchCollectionsResult>(
      transport::Get("/v1/stores/itch/collections"), "GET /v1/stores/itch/collections",
      Shape::Array, [](ItchCollectionsResult& result, const json& body) {
        for (const json& entry : body) {
          if (!entry.is_object()) continue;
          result.collections.push_back({.id = entry.value("id", std::int64_t{0}),
                                        .title = entry.value("title", std::string()),
                                        .games_count = entry.value("games_count", std::int64_t{0}),
                                        .own = entry.value("own", false)});
        }
      });
}

StoreActionResult AddItchCollectionSync(const std::string& link) {
  const transport::Reply reply =
      transport::PostJson("/v1/stores/itch/collections", {{"link", link}});
  return {reply.ok, reply.error};
}

StoreActionResult RemoveItchCollectionSync(std::int64_t id) {
  const transport::Reply reply =
      transport::Delete("/v1/stores/itch/collections/" + std::to_string(id));
  return {reply.ok, reply.error};
}

LoginUrlResult BeginStoreLoginSync(const std::string& source) {
  const std::string path = StorePath(source, "login/begin");
  return ReadReply<LoginUrlResult>(
      transport::Post(path, {.read_timeout = std::chrono::seconds(30)}), "POST " + path,
      Shape::Object, [](LoginUrlResult& result, const json& body) {
        result.url = body.value("url", std::string());
        if (result.url.empty()) throw std::runtime_error("no url");
      });
}

LaunchersResult GetLaunchersSync() {
  return ReadReply<LaunchersResult>(
      transport::Get("/v1/launchers"), "GET /v1/launchers", Shape::Array,
      [](LaunchersResult& result, const json& body) {
        for (const json& entry : body) {
          if (!entry.is_object()) continue;
          result.launchers.push_back(
              {.id = entry.value("id", std::string()),
               .name = entry.value("name", std::string()),
               .game_id = entry.value("game_id", std::string()),
               .installed = entry.value("installed", false),
               .install_state = entry.value("install_state", std::string()),
               .interactive_install = entry.value("interactive_install", false),
               .packages = entry.value("packages", std::string()),
               .prefix = entry.value("prefix", std::string()),
               .runner_ref = entry.value("runner_ref", std::string()),
               .error = entry.value("error", std::string())});
        }
      });
}

StoreActionResult InstallLauncherSync(const std::string& id) {
  const transport::Reply reply = transport::Post("/v1/launchers/" + PercentEncode(id) + "/install");
  return {reply.ok, reply.error};
}

StoreActionResult OpenLauncherSync(const std::string& id) {
  const transport::Reply reply =
      transport::PostJson("/v1/launchers/" + PercentEncode(id) + "/open", json::object());
  return {reply.ok, reply.error};
}

}  // namespace

void GetStoreStatusAsync(QObject* context, const std::string& source,
                         std::function<void(StoreStatusResult)> callback) {
  async::Run(
      context, [source] { return GetStoreStatusSync(source); }, std::move(callback),
      async::Lane::Slow);
}

void SetupStoreToolAsync(QObject* context, const std::string& source,
                         std::function<void(StoreActionResult)> callback) {
  RunJob<StoreActionResult>(
      context, "setup",
      [source](const std::string& query) {
        return transport::Post(StorePath(source, "setup") + query);
      },
      [](StoreActionResult&, const json&) {}, std::move(callback));
}

void SignInStoreAsync(QObject* context, const std::string& source, const std::string& credential,
                      std::function<void(StoreActionResult)> callback) {
  async::Run(
      context, [source, credential] { return SignInStoreSync(source, credential); },
      std::move(callback), async::Lane::Slow);
}

void SignOutStoreAsync(QObject* context, const std::string& source,
                       std::function<void(StoreActionResult)> callback) {
  async::Run(context, [source] { return SignOutStoreSync(source); }, std::move(callback));
}

void ImportStoreAsync(QObject* context, const std::string& source,
                      std::function<void(StoreImportResult)> callback) {
  RunJob<StoreImportResult>(
      context, "import",
      [source](const std::string& query) {
        return transport::Post(StorePath(source, "import") + query);
      },
      FillAddedUpdated<StoreImportResult>, std::move(callback));
}

void GetStoreLibraryAsync(QObject* context, const std::string& source, bool fresh,
                          std::function<void(StoreLibraryResult)> callback) {
  async::Run(
      context, [source, fresh] { return GetStoreLibrarySync(source, fresh); }, std::move(callback),
      async::Lane::Slow);
}

void InstallStoreTitleAsync(QObject* context, const std::string& source, const std::string& ref,
                            bool update, std::function<void(StoreActionResult)> callback) {
  async::Run(
      context, [source, ref, update] { return InstallStoreTitleSync(source, ref, update); },
      std::move(callback));
}

void PauseStoreInstallAsync(QObject* context, const std::string& source, const std::string& ref,
                            std::function<void(StoreActionResult)> callback) {
  async::Run(
      context, [source, ref] { return PauseStoreInstallSync(source, ref); }, std::move(callback));
}

void DiscardPausedInstallAsync(QObject* context, const std::string& source, const std::string& ref,
                               std::function<void(StoreActionResult)> callback) {
  async::Run(
      context, [source, ref] { return DiscardPausedInstallSync(source, ref); }, std::move(callback));
}

void ListPausedInstallsAsync(QObject* context, std::function<void(PausedInstallsResult)> callback) {
  async::Run(context, [] { return ListPausedInstallsSync(); }, std::move(callback));
}

void QueueTitleArtworkAsync(QObject* context, const std::string& source,
                            std::vector<StoreTitle> titles,
                            std::function<void(StoreActionResult)> callback) {
  async::Run(
      context,
      [source, titles = std::move(titles)] { return QueueTitleArtworkSync(source, titles); },
      std::move(callback));
}

void GetRemovalPlanAsync(QObject* context, const std::string& source,
                         std::function<void(RemovalPlanResult)> callback) {
  async::Run(context, [source] { return GetRemovalPlanSync(source); }, std::move(callback));
}

void RemoveSourceAsync(QObject* context, const std::string& source,
                       std::function<void(RemoveSourceResult)> callback) {
  RunJob<RemoveSourceResult>(
      context, "remove_source",
      [source](const std::string& query) {
        return transport::Post("/v1/sources/" + PercentEncode(source) + "/remove" + query);
      },
      FillRemoveSource, std::move(callback));
}

void GetSourceRunnerAsync(QObject* context, const std::string& source,
                          std::function<void(SourceRunnerResult)> callback) {
  async::Run(context, [source] { return GetSourceRunnerSync(source); }, std::move(callback));
}

void SetSourceRunnerAsync(QObject* context, const std::string& source,
                          const std::string& runner_ref, bool apply_to_games,
                          std::function<void(SourceRunnerResult)> callback) {
  async::Run(
      context,
      [source, runner_ref, apply_to_games] {
        return SetSourceRunnerSync(source, runner_ref, apply_to_games);
      },
      std::move(callback));
}

void GetItchCollectionsAsync(QObject* context,
                             std::function<void(ItchCollectionsResult)> callback) {
  async::Run(context, [] { return GetItchCollectionsSync(); }, std::move(callback));
}

void AddItchCollectionAsync(QObject* context, const std::string& link,
                            std::function<void(StoreActionResult)> callback) {
  async::Run(context, [link] { return AddItchCollectionSync(link); }, std::move(callback));
}

void RemoveItchCollectionAsync(QObject* context, std::int64_t id,
                               std::function<void(StoreActionResult)> callback) {
  async::Run(context, [id] { return RemoveItchCollectionSync(id); }, std::move(callback));
}

void GetHumbleLibraryAsync(QObject* context, std::function<void(HumbleLibraryResult)> callback) {
  async::Run(
      context, [] { return GetHumbleLibrarySync(); }, std::move(callback), async::Lane::Slow);
}

void DownloadHumbleBundleAsync(QObject* context, const std::string& bundle_key,
                               std::function<void(HumbleDownloadResult)> callback) {
  RunJob<HumbleDownloadResult>(
      context, "download",
      [bundle_key](const std::string& query) {
        return transport::PostJson("/v1/stores/humble/download" + query,
                                   {{"bundle_key", bundle_key}});
      },
      [](HumbleDownloadResult& result, const json& body) {
        result.path = body.value("path", std::string());
      },
      std::move(callback));
}

void BeginStoreLoginAsync(QObject* context, const std::string& source,
                          std::function<void(LoginUrlResult)> callback) {
  async::Run(context, [source] { return BeginStoreLoginSync(source); }, std::move(callback));
}

void GetLaunchersAsync(QObject* context, std::function<void(LaunchersResult)> callback) {
  async::Run(context, [] { return GetLaunchersSync(); }, std::move(callback));
}

void InstallLauncherAsync(QObject* context, const std::string& id,
                          std::function<void(StoreActionResult)> callback) {
  async::Run(context, [id] { return InstallLauncherSync(id); }, std::move(callback));
}

void ImportLauncherAsync(QObject* context, const std::string& id,
                         std::function<void(StoreImportResult)> callback) {
  RunJob<StoreImportResult>(
      context, "import",
      [id](const std::string& query) {
        return transport::Post("/v1/launchers/" + PercentEncode(id) + "/import" + query);
      },
      FillAddedUpdated<StoreImportResult>, std::move(callback));
}

void OpenLauncherAsync(QObject* context, const std::string& id,
                       std::function<void(StoreActionResult)> callback) {
  async::Run(context, [id] { return OpenLauncherSync(id); }, std::move(callback));
}

}  // namespace mira_gui::api
