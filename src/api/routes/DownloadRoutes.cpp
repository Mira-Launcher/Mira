#include <httplib.h>

#include "api/Http.h"
#include "api/Routes.h"
#include "api/Services.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

void SendDone(Response& res, const Result<void>& result) {
  if (!result) {
    const bool unknown = result.error().code == "unknown_download";
    return SendError(res, unknown ? 404 : 409, result.error());
  }
  SendJson(res, json::object());
}

}  // namespace

void RegisterDownloadRoutes(Router& http, Services& s) {
  http.Get("/v1/downloads",
           [&s](const Request&, Response& res) { SendJson(res, s.downloads.List()); });

  http.Post("/v1/downloads/check", [&s](const Request& req, Response& res) {
    const auto body = BodyObject(req, res, R"({"links": ["..."]})");
    if (!body) return;
    const auto links = StringList(*body, "links");
    if (!links) return SendError(res, 400, "invalid_body", "links must be a list of strings");
    const auto checked = s.downloads.Check(*links);
    if (!checked) return SendError(res, 400, checked.error());
    SendJson(res, *checked);
  });

  http.Post("/v1/downloads", [&s](const Request& req, Response& res) {
    const auto body =
        BodyObject(req, res, R"({"name": "...", "links": ["..."], "password": "..."})");
    if (!body) return;
    const auto links = StringList(*body, "links");
    if (!links) return SendError(res, 400, "invalid_body", "links must be a list of strings");
    const auto added = s.downloads.Add(body->value("name", std::string()), *links,
                                       body->value("password", std::string()));
    if (!added) return SendError(res, 400, added.error());
    SendJson(res, *added, 201);
  });

  http.Post(R"(/v1/downloads/([^/]+)/pause)", [&s](const Request& req, Response& res) {
    SendDone(res, s.downloads.Pause(req.matches[1]));
  });
  http.Post(R"(/v1/downloads/([^/]+)/resume)", [&s](const Request& req, Response& res) {
    SendDone(res, s.downloads.Resume(req.matches[1]));
  });
  http.Post(R"(/v1/downloads/([^/]+)/retry)", [&s](const Request& req, Response& res) {
    SendDone(res, s.downloads.Retry(req.matches[1]));
  });
  http.Post(R"(/v1/downloads/([^/]+)/front)", [&s](const Request& req, Response& res) {
    SendDone(res, s.downloads.MoveToFront(req.matches[1]));
  });
  http.Post(R"(/v1/downloads/([^/]+)/password)", [&s](const Request& req, Response& res) {
    const auto body = BodyObject(req, res, R"({"password": "..."})");
    if (!body) return;
    SendDone(res,
             s.downloads.SubmitPassword(req.matches[1], body->value("password", std::string())));
  });
  http.Delete(R"(/v1/downloads/([^/]+))", [&s](const Request& req, Response& res) {
    SendDone(res, s.downloads.Remove(req.matches[1]));
  });
}

}  // namespace mira::api
