#pragma once

#include <QObject>
#include <functional>
#include <json.hpp>
#include <string>
#include <vector>

#include "../Types.h"

// Link downloads (docs/api.md#downloads).
namespace mira_gui::api {

// One download as mirad sends it, in the list and in download.changed.
DownloadInfo ParseDownload(const nlohmann::json& entry);

void GetDownloadsAsync(QObject* context, std::function<void(DownloadsResult)> callback);
void CheckLinksAsync(QObject* context, const std::vector<std::string>& links,
                     std::function<void(LinkCheckResult)> callback);
void AddDownloadAsync(QObject* context, const std::string& name,
                      const std::vector<std::string>& links, const std::string& password,
                      std::function<void(StoreActionResult)> callback);
// `action` is pause, resume, retry or front.
void DownloadActionAsync(QObject* context, const std::string& id, const std::string& action,
                         std::function<void(StoreActionResult)> callback);
void SubmitDownloadPasswordAsync(QObject* context, const std::string& id,
                                 const std::string& password,
                                 std::function<void(StoreActionResult)> callback);
void GetDownloadPasswordsAsync(QObject* context,
                               std::function<void(DownloadPasswordsResult)> callback);
void SetDownloadPasswordsAsync(QObject* context, const std::vector<std::string>& passwords,
                               std::function<void(StoreActionResult)> callback);
void RemoveDownloadAsync(QObject* context, const std::string& id,
                         std::function<void(StoreActionResult)> callback);

}  // namespace mira_gui::api
