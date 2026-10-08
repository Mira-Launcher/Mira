#include "api/Routes.h"

#include <httplib.h>

#include "api/Http.h"
#include "api/Services.h"
#include "config/Schema.h"
#include "runner/GameMode.h"

namespace mira::api {
namespace {
using httplib::Request;
using httplib::Response;
using nlohmann::json;

// The settings keys that changed (names only: values can be secrets) and the whole frontend
// table, so a client applies another client's change without asking again.
void PublishConfigChanged(Services& s, json keys) {
  s.events.Publish("config.changed", {{"keys", std::move(keys)}, {"frontend", s.config.FrontendSettings()}});
}

}  // namespace

void RegisterConfigRoutes(httplib::Server& http, Services& s) {
  http.Get("/v1/jobs/([A-Za-z0-9_-]+)", [&s](const Request& req, Response& res) {
    const auto job = s.jobs.Find(req.matches[1].str());
    if (!job) return SendError(res, 404, "job_not_found", "no such job, or it finished too long ago");
    SendJson(res, *job);
  });

  http.Post("/v1/jobs/([A-Za-z0-9_-]+)/cancel", [&s](const Request& req, Response& res) {
    if (auto cancelled = s.jobs.Cancel(req.matches[1].str()); !cancelled) {
      return SendError(res, cancelled.error().code == "job_not_found" ? 404 : 409, cancelled.error());
    }
    SendJson(res, {{"status", "cancelling"}});
  });

  http.Get("/v1/health", [](const Request&, Response& res) {
    SendJson(res, {{"status", "ok"}, {"api", kApiVersion}});
  });

  http.Get("/v1/gamemode/status", [](const Request&, Response& res) {
    SendJson(res, {{"installed", gamemode::IsInstalled()}, {"daemon_running", gamemode::IsDaemonRunning()}});
  });

  // --- settings -----------------------------------------------------------

  http.Get("/v1/config", [&s](const Request&, Response& res) {
    json body = s.config.Document();
    body["frontend"] = s.config.FrontendSettings();
    SendJson(res, std::move(body));
  });

  http.Get("/v1/config/schema", [](const Request&, Response& res) {
    json entries = json::array();
    for (const config::Entry& entry : config::Schema::Instance().Entries()) {
      entries.push_back({
          {"key", entry.key},
          {"label", entry.label},
          {"type", config::ToString(entry.type)},
          {"default", entry.default_value},
          {"scope", config::ToString(entry.scope)},
          {"doc", entry.doc},
          {"category", entry.category},
          {"group", entry.group},
          {"group_label", entry.group_label},
      });
      // Present only when there's a shape to describe.
      if (!entry.constraint.one_of.empty()) entries.back()["one_of"] = entry.constraint.one_of;
      if (entry.constraint.minimum) entries.back()["minimum"] = *entry.constraint.minimum;
      if (entry.constraint.maximum) entries.back()["maximum"] = *entry.constraint.maximum;
      if (!entry.game_doc.empty()) entries.back()["game_doc"] = entry.game_doc;
      if (entry.is_secret) entries.back()["is_secret"] = true;
      if (entry.is_runner_ref) entries.back()["is_runner_ref"] = true;
      if (!entry.link.empty()) entries.back()["link"] = entry.link;
      if (!entry.keywords.empty()) entries.back()["keywords"] = entry.keywords;
      if (entry.group_collapsed) entries.back()["group_collapsed"] = true;
      if (entry.group_resettable) entries.back()["group_resettable"] = true;
      if (!entry.source.empty()) entries.back()["source"] = entry.source;
      if (entry.path != config::PathKind::None) {
        entries.back()["path"] = entry.path == config::PathKind::Folder ? "folder" : "file";
      }
    }
    SendJson(res, std::move(entries));
  });

  http.Patch("/v1/config", [&s](const Request& req, Response& res) {
    json patch = json::parse(req.body, nullptr, false);
    if (patch.is_discarded()) return SendError(res, 400, "invalid_json", "body is not valid JSON");
    Result<void> result = s.config.Patch(patch);
    if (result) s.SyncDesktopEntries();
    if (result && patch.is_object() && patch.contains("library_roots") && s.on_roots_changed) s.on_roots_changed();
    if (result) {
      json keys = json::array();
      for (const config::Entry& entry : config::Schema::Instance().Entries()) {
        if (patch.contains(config::Schema::Pointer(entry.key))) keys.push_back(entry.key);
      }
      PublishConfigChanged(s, std::move(keys));
    }
    SendResult(res, result);
  });

  http.Post("/v1/config/reset", [&s](const Request& req, Response& res) {
    Result<void> result;
    bool roots_reset = true;
    if (auto it = req.params.find("key"); it != req.params.end()) {
      result = s.config.Reset(it->second);
      roots_reset = it->second == "library_roots";
    } else {
      result = s.config.ResetAll();
    }
    if (result) s.SyncDesktopEntries();
    if (result && roots_reset && s.on_roots_changed) s.on_roots_changed();
    if (result) {
      json keys = json::array();
      if (auto it = req.params.find("key"); it != req.params.end()) {
        keys.push_back(it->second);
      } else {
        for (const config::Entry& entry : config::Schema::Instance().Entries()) keys.push_back(entry.key);
      }
      PublishConfigChanged(s, std::move(keys));
    }
    SendResult(res, result);
  });
}

}  // namespace mira::api
