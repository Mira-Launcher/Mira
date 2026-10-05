#include "Runners.h"

#include <chrono>
#include <json.hpp>
#include <utility>

#include "../Async.h"
#include "../Transport.h"
#include "Request.h"

namespace mira_gui::api {
namespace {

using nlohmann::json;

RunnersResult GetRunnersSync() {
  return ReadReply<RunnersResult>(transport::Get("/v1/runners"), "GET /v1/runners", Shape::Array,
                                  [](RunnersResult& result, const json& body) {
                                    for (const json& entry : body) {
                                      RunnerInfo runner;
                                      runner.kind = entry.value("kind", std::string());
                                      runner.name = entry.value("name", std::string());
                                      runner.version = entry.value("version", std::string());
                                      runner.reference = entry.value("reference", std::string());
                                      runner.path = entry.value("path", std::string());
                                      runner.label = entry.value("label", runner.name);
                                      runner.source = entry.value("source", std::string());
                                      runner.removable = entry.value("removable", false);
                                      result.runners.push_back(std::move(runner));
                                    }
                                  });
}

RunnerCatalogResult GetRunnerCatalogSync(const std::string& kind, const std::string& source) {
  std::string path = "/v1/runners/catalog?kind=" + PercentEncode(kind);
  if (!source.empty()) path += "&source=" + source;
  // Leaves the machine (GitHub releases), so the default timeout is nowhere
  // near enough.
  return ReadReply<RunnerCatalogResult>(
      transport::Get(path, {.read_timeout = std::chrono::seconds(30)}), "GET /v1/runners/catalog",
      Shape::Array, [](RunnerCatalogResult& result, const json& body) {
        for (const json& entry : body) {
          RunnerRelease release;
          release.tag = entry.value("tag", std::string());
          release.name = entry.value("name", release.tag);
          release.label = entry.value("label", release.name);
          release.source = entry.value("source", std::string());
          release.installed = entry.value("installed", false);
          release.asset_name = entry.value("asset_name", std::string());
          release.size_bytes = entry.value("size_bytes", std::int64_t{0});
          release.published_at = entry.value("published_at", std::string());
          release.has_checksum = entry.value("has_checksum", false);
          result.releases.push_back(std::move(release));
        }
      });
}

RunnerDownloadResult DownloadRunnerSync(const std::string& kind, const std::string& tag,
                                        const std::string& source) {
  // Checks GitHub for the release before answering.
  const transport::Reply reply = transport::PostJson(
      "/v1/runners/download", json{{"kind", kind}, {"tag", tag}, {"source", source}},
      {.read_timeout = std::chrono::seconds(30)});
  return {reply.ok, reply.error};
}

RunnerSourcesResult ListRunnerSourcesSync(const std::string& kind) {
  return ReadReply<RunnerSourcesResult>(
      transport::Get("/v1/runners/sources?kind=" + PercentEncode(kind)), "GET /v1/runners/sources",
      Shape::Array, [](RunnerSourcesResult& result, const json& body) {
        for (const json& entry : body) {
          result.sources.push_back(
              {entry.value("id", std::string()), entry.value("label", std::string())});
        }
      });
}

RunnerUpdatesResult GetRunnerUpdatesSync() {
  return ReadReply<RunnerUpdatesResult>(
      transport::Get("/v1/runners/updates", {.read_timeout = std::chrono::seconds(60)}),
      "GET /v1/runners/updates", Shape::Array, [](RunnerUpdatesResult& result, const json& body) {
        for (const json& entry : body) {
          result.updates.push_back(
              {entry.value("reference", std::string()), entry.value("source", std::string()),
               entry.value("tag", std::string()), entry.value("name", std::string()),
               entry.value("label", std::string())});
        }
      });
}

RunnerDownloadResult UpdateRunnerSync(const std::string& reference) {
  const transport::Reply reply =
      transport::PostJson("/v1/runners/update", json{{"reference", reference}},
                          {.read_timeout = std::chrono::seconds(60)});
  return {reply.ok, reply.error};
}

RunnerToolsResult ListRunnerToolsSync() {
  return ReadReply<RunnerToolsResult>(
      transport::Get("/v1/runners/tools"), "GET /v1/runners/tools", Shape::Array,
      [](RunnerToolsResult& result, const json& body) {
        for (const json& entry : body) {
          result.tools.push_back(
              {entry.value("id", std::string()), entry.value("label", std::string()),
               entry.value("doc", std::string()), entry.value("path", std::string()),
               entry.value("installed", false)});
        }
      });
}

RunnerSchemaResult GetRunnerSchemaSync(const std::string& kind) {
  const std::string path = "/v1/runners/" + PercentEncode(kind) + "/schema";
  return ReadReply<RunnerSchemaResult>(
      transport::Get(path), "GET " + path, Shape::Array,
      [](RunnerSchemaResult& result, const json& body) {
        for (const json& entry : body) {
          if (!entry.is_object()) continue;
          const std::string key = entry.value("key", std::string());
          result.options.push_back(
              {key, entry.value("label", key), entry.value("doc", std::string())});
        }
      });
}

RunnerDownloadResult SetupRunnerToolSync(const std::string& id) {
  const transport::Reply reply = transport::Post("/v1/runners/tools/" + id + "/setup",
                                                 {.read_timeout = std::chrono::seconds(30)});
  return {reply.ok, reply.error};
}

RunnerRemoveResult DeleteRunnerSync(const std::string& kind, const std::string& name) {
  const transport::Reply reply =
      transport::Delete("/v1/runners/" + kind + ":" + PercentEncode(name));
  return {reply.ok, reply.error};
}

}  // namespace

void ListRunnersAsync(QObject* context, std::function<void(RunnersResult)> callback) {
  async::Run(context, [] { return GetRunnersSync(); }, std::move(callback));
}

void GetRunnerCatalogAsync(QObject* context, const std::string& kind, const std::string& source,
                           std::function<void(RunnerCatalogResult)> callback) {
  async::Run(
      context, [kind, source] { return GetRunnerCatalogSync(kind, source); }, std::move(callback),
      async::Lane::Slow);
}

void DownloadRunnerAsync(QObject* context, const std::string& kind, const std::string& tag,
                         const std::string& source,
                         std::function<void(RunnerDownloadResult)> callback) {
  async::Run(
      context, [kind, tag, source] { return DownloadRunnerSync(kind, tag, source); },
      std::move(callback), async::Lane::Slow);
}

void ListRunnerSourcesAsync(QObject* context, const std::string& kind,
                            std::function<void(RunnerSourcesResult)> callback) {
  async::Run(context, [kind] { return ListRunnerSourcesSync(kind); }, std::move(callback));
}

void GetRunnerUpdatesAsync(QObject* context, std::function<void(RunnerUpdatesResult)> callback) {
  async::Run(
      context, [] { return GetRunnerUpdatesSync(); }, std::move(callback), async::Lane::Slow);
}

void UpdateRunnerAsync(QObject* context, const std::string& reference,
                       std::function<void(RunnerDownloadResult)> callback) {
  async::Run(
      context, [reference] { return UpdateRunnerSync(reference); }, std::move(callback),
      async::Lane::Slow);
}

void ListRunnerToolsAsync(QObject* context, std::function<void(RunnerToolsResult)> callback) {
  async::Run(context, [] { return ListRunnerToolsSync(); }, std::move(callback));
}

void GetRunnerSchemaAsync(QObject* context, const std::string& kind,
                          std::function<void(RunnerSchemaResult)> callback) {
  async::Run(context, [kind] { return GetRunnerSchemaSync(kind); }, std::move(callback));
}

void SetupRunnerToolAsync(QObject* context, const std::string& id,
                          std::function<void(RunnerDownloadResult)> callback) {
  async::Run(
      context, [id] { return SetupRunnerToolSync(id); }, std::move(callback), async::Lane::Slow);
}

void DeleteRunnerAsync(QObject* context, const std::string& kind, const std::string& name,
                       std::function<void(RunnerRemoveResult)> callback) {
  async::Run(context, [kind, name] { return DeleteRunnerSync(kind, name); }, std::move(callback));
}

}  // namespace mira_gui::api
