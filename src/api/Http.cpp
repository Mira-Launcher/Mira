#include "api/Http.h"

#include <format>

#include <httplib.h>

#include "api/EventBus.h"

namespace mira::api {
using nlohmann::json;

json ErrorBody(const Error& error) { return {{"error", ErrorJson(error)}}; }

void SendError(httplib::Response& res, int status, std::string_view code, std::string_view message) {
  res.status = status;
  res.set_content(ErrorBody(Error{std::string(code), std::string(message), {}, {}}).dump(), "application/json");
}

void SendError(httplib::Response& res, int status, const Error& error) {
  res.status = status;
  res.set_content(ErrorBody(error).dump(), "application/json");
}

void SendJson(httplib::Response& res, json body, int status) {
  res.status = status;
  res.set_content(body.dump(), "application/json");
}

void SendResult(httplib::Response& res, const Result<void>& result) {
  if (result) {
    SendJson(res, json::object());
  } else {
    SendError(res, 400, result.error());
  }
}

void SendStoreError(httplib::Response& res, const Error& error) {
  SendError(res, error.code == "game_not_found" ? 404 : 500, error);
}

std::optional<json> BodyObject(const httplib::Request& req, httplib::Response& res, std::string_view expected,
                               bool allow_empty) {
  if (allow_empty && req.body.empty()) return json::object();
  json body = json::parse(req.body, nullptr, false);
  if (body.is_discarded()) {
    SendError(res, 400, "invalid_json", "body is not valid JSON");
    return std::nullopt;
  }
  if (!body.is_object()) {
    SendError(res, 400, "invalid_body", std::format("expected {}", expected));
    return std::nullopt;
  }
  return body;
}

std::string Param(const httplib::Request& req, const char* name, const std::string& fallback) {
  return req.has_param(name) ? req.get_param_value(name) : fallback;
}

bool BoolParam(const httplib::Request& req, const char* name) {
  const std::string value = Param(req, name);
  return value == "1" || value == "true" || value == "yes";
}

std::optional<std::vector<std::string>> StringList(const json& body, const char* key) {
  if (!body.contains(key)) return std::vector<std::string>{};
  if (!body[key].is_array()) return std::nullopt;
  std::vector<std::string> out;
  for (const json& item : body[key]) {
    if (!item.is_string()) return std::nullopt;
    out.push_back(item.get<std::string>());
  }
  return out;
}

bool IsSafeRef(const std::string& ref) {
  return !ref.empty() && !ref.starts_with('-') && ref.find('/') == std::string::npos &&
         ref.find('\0') == std::string::npos;
}

Error GameRunningError(const std::string& id) {
  return Error{"game_running", std::format("\"{}\" is running", id), "Stop the game first.", {}};
}

json BatchFailure(const std::string& id, const Error& error) {
  json body = ErrorBody(error);
  body["id"] = id;
  return body;
}

}  // namespace mira::api
