#include "Runners.h"

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

RunnersResult GetRunnersSync() {
  RunnersResult result;
  const transport::Reply reply = transport::Get("/v1/runners");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/runners");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) {
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
  return result;
}

RunnerCatalogResult GetRunnerCatalogSync(const std::string& kind, const std::string& source) {
  RunnerCatalogResult result;
  // Leaves the machine (GitHub releases), so the default timeout is nowhere
  // near enough.
  std::string path = "/v1/runners/catalog?kind=" + PercentEncode(kind);
  if (!source.empty()) path += "&source=" + source;
  const transport::Reply reply = transport::Get(path, {.read_timeout = std::chrono::seconds(30)});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/runners/catalog");
    return result;
  }

  result.ok = true;
  for (const json& entry : reply.body) {
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
  return result;
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
  RunnerSourcesResult result;
  const transport::Reply reply = transport::Get("/v1/runners/sources?kind=" + PercentEncode(kind));
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/runners/sources");
    return result;
  }
  result.ok = true;
  for (const json& entry : reply.body) {
    result.sources.push_back(
        {entry.value("id", std::string()), entry.value("label", std::string())});
  }
  return result;
}

RunnerUpdatesResult GetRunnerUpdatesSync() {
  RunnerUpdatesResult result;
  const transport::Reply reply =
      transport::Get("/v1/runners/updates", {.read_timeout = std::chrono::seconds(60)});
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/runners/updates");
    return result;
  }
  result.ok = true;
  for (const json& entry : reply.body) {
    result.updates.push_back({entry.value("reference", std::string()),
                              entry.value("source", std::string()),
                              entry.value("tag", std::string()), entry.value("name", std::string()),
                              entry.value("label", std::string())});
  }
  return result;
}

RunnerDownloadResult UpdateRunnerSync(const std::string& reference) {
  const transport::Reply reply =
      transport::PostJson("/v1/runners/update", json{{"reference", reference}},
                          {.read_timeout = std::chrono::seconds(60)});
  return {reply.ok, reply.error};
}

RunnerToolsResult ListRunnerToolsSync() {
  RunnerToolsResult result;
  const transport::Reply reply = transport::Get("/v1/runners/tools");
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET /v1/runners/tools");
    return result;
  }
  result.ok = true;
  for (const json& entry : reply.body) {
    result.tools.push_back({entry.value("id", std::string()), entry.value("label", std::string()),
                            entry.value("doc", std::string()), entry.value("path", std::string()),
                            entry.value("installed", false)});
  }
  return result;
}

RunnerSchemaResult GetRunnerSchemaSync(const std::string& kind) {
  RunnerSchemaResult result;
  const std::string path = "/v1/runners/" + PercentEncode(kind) + "/schema";
  const transport::Reply reply = transport::Get(path);
  if (!reply.ok) {
    result.error = reply.error;
    return result;
  }
  if (!reply.body.is_array()) {
    result.error = transport::UnexpectedResponse("GET " + path);
    return result;
  }
  result.ok = true;
  for (const json& entry : reply.body) {
    if (!entry.is_object()) continue;
    const std::string key = entry.value("key", std::string());
    result.options.push_back({key, entry.value("label", key), entry.value("doc", std::string())});
  }
  return result;
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
