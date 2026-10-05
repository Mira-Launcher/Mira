#pragma once

#include <QImage>
#include <QObject>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../Types.h"

// Proton and Wine builds, their sources and the tools runners need.
// Each call runs on a worker thread and delivers its result on the main thread
// (client/Async.h), so the UI never blocks on the socket. See docs/api.md.
namespace mira_gui::api {

// GET /v1/runners.
void ListRunnersAsync(QObject* context, std::function<void(RunnersResult)> callback);

// GET /v1/runners/catalog?kind=proton|wine&source=: what is available
// to install from one source (empty: the kind's preferred one). Unlike
// everything else here this goes out to the GitHub API, so it has real
// network latency and its own longer timeout.
void GetRunnerCatalogAsync(QObject* context, const std::string& kind, const std::string& source,
                           std::function<void(RunnerCatalogResult)> callback);

// POST /v1/runners/download. Returns 202 as soon as the download starts;
// the outcome arrives as a runners.download.finished/.failed event, since
// a build can be 500+ MB.
void DownloadRunnerAsync(QObject* context, const std::string& kind, const std::string& tag,
                         const std::string& source,
                         std::function<void(RunnerDownloadResult)> callback);

// GET /v1/runners/sources?kind=.
void ListRunnerSourcesAsync(QObject* context, const std::string& kind,
                            std::function<void(RunnerSourcesResult)> callback);

// GET /v1/runners/updates. Also reaches GitHub, cached by mirad.
void GetRunnerUpdatesAsync(QObject* context, std::function<void(RunnerUpdatesResult)> callback);

// POST /v1/runners/update. Starts like a download; games on the old build
// move to the new one when it finishes.
void UpdateRunnerAsync(QObject* context, const std::string& reference,
                       std::function<void(RunnerDownloadResult)> callback);

// GET /v1/runners/tools and POST /v1/runners/tools/{id}/setup (reported as
// <id>.setup.* events).
void ListRunnerToolsAsync(QObject* context, std::function<void(RunnerToolsResult)> callback);

// GET /v1/runners/{kind}/schema: the options a kind's runner_config takes.
void GetRunnerSchemaAsync(QObject* context, const std::string& kind,
                          std::function<void(RunnerSchemaResult)> callback);

void SetupRunnerToolAsync(QObject* context, const std::string& id,
                          std::function<void(RunnerDownloadResult)> callback);

// DELETE /v1/runners/{kind}:{name}. Synchronous: 200 once the build's
// files are actually gone.
void DeleteRunnerAsync(QObject* context, const std::string& kind, const std::string& name,
                       std::function<void(RunnerRemoveResult)> callback);

}  // namespace mira_gui::api
