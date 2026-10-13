#include <httplib.h>

#include <format>

#include "api/Http.h"
#include "api/Routes.h"
#include "api/Services.h"

namespace mira::api {

using httplib::Request;
using httplib::Response;
using nlohmann::json;

void RegisterHiddenRoutes(Router& http, Services& s) {
  http.Get("/v1/hidden", [&s](const Request&, Response& res) { SendJson(res, s.HiddenState()); });

  // Sets, changes or (with "password": "") removes the password on hidden games.
  http.Put("/v1/hidden/password", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"current"?: "...", "password": "..."})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    const json& b = *body;
    if (!b.contains("password") || !b["password"].is_string() ||
        (b.contains("current") && !b["current"].is_string())) {
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    }
    const auto set =
        s.hidden.SetPassword(b.value("current", std::string()), b["password"].get<std::string>());
    if (!set) return SendError(res, set.error().code == "wrong_password" ? 403 : 500, set.error());
    s.PublishHiddenState();
    SendJson(res, s.HiddenState());
  });

  http.Post("/v1/hidden/unlock", [&s](const Request& req, Response& res) {
    constexpr std::string_view kShape = R"({"password": "..."})";
    const auto body = BodyObject(req, res, kShape);
    if (!body) return;
    if (!body->contains("password") || !(*body)["password"].is_string()) {
      return SendError(res, 400, "invalid_body", std::format("expected {}", kShape));
    }
    const bool was_locked = s.hidden.Locked();
    if (auto unlocked = s.hidden.Unlock((*body)["password"].get<std::string>()); !unlocked) {
      return SendError(res, 403, unlocked.error());
    }
    if (was_locked) s.PublishHiddenState();
    SendJson(res, s.HiddenState());
  });

  http.Post("/v1/hidden/lock", [&s](const Request&, Response& res) {
    s.LockHidden();
    SendJson(res, s.HiddenState());
  });

  // A client left hidden mode: locks when hidden.lock_on_leave is on.
  http.Post("/v1/hidden/left", [&s](const Request&, Response& res) {
    if (s.config.GetBool("hidden.lock_on_leave")) s.LockHidden();
    SendJson(res, s.HiddenState());
  });

  // Someone is using Mira: restarts hidden.lock_after_minutes' count.
  http.Post("/v1/hidden/activity", [&s](const Request&, Response& res) {
    s.hidden.Touch();
    res.status = 204;
  });
}

}  // namespace mira::api
