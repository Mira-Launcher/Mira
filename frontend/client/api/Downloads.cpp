#include "Downloads.h"

#include "../Async.h"
#include "../Transport.h"
#include "Request.h"

namespace mira_gui::api {
using nlohmann::json;
namespace {

std::string Path(const std::string& id) {
  return "/v1/downloads/" + PercentEncode(id);
}

void RunAction(QObject* context, std::string endpoint, std::function<transport::Reply()> send,
               std::function<void(StoreActionResult)> callback) {
  async::Run(
      context,
      [endpoint = std::move(endpoint), send = std::move(send)] {
        return ReadReply<StoreActionResult>(send(), endpoint, Shape::Object,
                                            [](StoreActionResult&, const json&) {});
      },
      std::move(callback));
}

}  // namespace

DownloadInfo ParseDownload(const json& entry) {
  DownloadInfo info;
  info.id = entry.value("id", std::string());
  info.name = entry.value("name", std::string());
  info.state = entry.value("state", std::string());
  info.total = entry.value("total", std::int64_t{0});
  info.done = entry.value("done", std::int64_t{0});
  info.speed = entry.value("speed", std::int64_t{0});
  if (entry.contains("error") && entry["error"].is_object()) {
    info.error.code = entry["error"].value("code", std::string());
    info.error.message = entry["error"].value("message", std::string());
    info.error.hint = entry["error"].value("hint", std::string());
  }
  if (entry.contains("files") && entry["files"].is_array()) {
    for (const json& item : entry["files"]) {
      DownloadFile file;
      file.name = item.value("name", std::string());
      file.source = item.value("source", std::string());
      file.total = item.value("total", std::int64_t{0});
      file.done = item.value("done", std::int64_t{0});
      file.state = item.value("state", std::string());
      info.files.push_back(std::move(file));
    }
  }
  return info;
}

void GetDownloadsAsync(QObject* context, std::function<void(DownloadsResult)> callback) {
  async::Run(
      context,
      [] {
        return ReadReply<DownloadsResult>(transport::Get("/v1/downloads"), "GET /v1/downloads",
                                          Shape::Array,
                                          [](DownloadsResult& result, const json& body) {
                                            for (const json& entry : body)
                                              result.downloads.push_back(ParseDownload(entry));
                                          });
      },
      std::move(callback));
}

void CheckLinksAsync(QObject* context, const std::vector<std::string>& links,
                     std::function<void(LinkCheckResult)> callback) {
  async::Run(
      context,
      [body = json{{"links", links}}] {
        return ReadReply<LinkCheckResult>(transport::PostJson("/v1/downloads/check", body),
                                          "POST /v1/downloads/check", Shape::Object,
                                          [](LinkCheckResult& result, const json& reply) {
                                            for (const json& item : reply.at("files")) {
                                              DownloadFile file;
                                              file.name = item.value("name", std::string());
                                              file.source = item.value("source", std::string());
                                              file.total = item.value("size", std::int64_t{0});
                                              result.files.push_back(std::move(file));
                                            }
                                          });
      },
      std::move(callback));
}

void AddDownloadAsync(QObject* context, const std::string& name,
                      const std::vector<std::string>& links, const std::string& password,
                      std::function<void(StoreActionResult)> callback) {
  RunAction(
      context, "POST /v1/downloads",
      [body = json{{"name", name}, {"links", links}, {"password", password}}] {
        return transport::PostJson("/v1/downloads", body);
      },
      std::move(callback));
}

void DownloadActionAsync(QObject* context, const std::string& id, const std::string& action,
                         std::function<void(StoreActionResult)> callback) {
  RunAction(
      context, "POST /v1/downloads/" + action,
      [path = Path(id) + "/" + action] { return transport::Post(path); }, std::move(callback));
}

void SubmitDownloadPasswordAsync(QObject* context, const std::string& id,
                                 const std::string& password,
                                 std::function<void(StoreActionResult)> callback) {
  RunAction(
      context, "POST /v1/downloads/password",
      [path = Path(id) + "/password", body = json{{"password", password}}] {
        return transport::PostJson(path, body);
      },
      std::move(callback));
}

void GetDownloadPasswordsAsync(QObject* context,
                               std::function<void(DownloadPasswordsResult)> callback) {
  async::Run(
      context,
      [] {
        return ReadReply<DownloadPasswordsResult>(
            transport::Get("/v1/downloads/passwords"), "GET /v1/downloads/passwords", Shape::Array,
            [](DownloadPasswordsResult& result, const json& body) {
              result.passwords = body.get<std::vector<std::string>>();
            });
      },
      std::move(callback));
}

void SetDownloadPasswordsAsync(QObject* context, const std::vector<std::string>& passwords,
                               std::function<void(StoreActionResult)> callback) {
  RunAction(
      context, "PUT /v1/downloads/passwords",
      [body = json{{"passwords", passwords}}] {
        return transport::Put("/v1/downloads/passwords", body.dump(), "application/json");
      },
      std::move(callback));
}

void RemoveDownloadAsync(QObject* context, const std::string& id,
                         std::function<void(StoreActionResult)> callback) {
  RunAction(
      context, "DELETE /v1/downloads", [path = Path(id)] { return transport::Delete(path); },
      std::move(callback));
}

}  // namespace mira_gui::api
