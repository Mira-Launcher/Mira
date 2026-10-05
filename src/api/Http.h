#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <json.hpp>

#include "core/Result.h"

namespace httplib {
class Server;
struct Request;
struct Response;
}  // namespace httplib

namespace mira::api {

// {"error": {code, message, hint?, fix?}}
nlohmann::json ErrorBody(const Error& error);

void SendError(httplib::Response& res, int status, std::string_view code, std::string_view message);
void SendError(httplib::Response& res, int status, const Error& error);
void SendJson(httplib::Response& res, nlohmann::json body, int status = 200);
// {} on success, 400 with the error otherwise.
void SendResult(httplib::Response& res, const Result<void>& result);
// A GameStore failure: an unknown id is 404, a failed save is the daemon's fault.
void SendStoreError(httplib::Response& res, const Error& error);

// The request body as a JSON object, or nullopt after a 400: invalid_json when it doesn't parse,
// invalid_body (saying `expected`) when it isn't an object. An empty body is {} when `allow_empty`.
std::optional<nlohmann::json> BodyObject(const httplib::Request& req, httplib::Response& res,
                                         std::string_view expected, bool allow_empty = false);

// A query parameter, or `fallback` when absent.
std::string Param(const httplib::Request& req, const char* name, const std::string& fallback = std::string());
// true for 1, true or yes; false when absent or anything else.
bool BoolParam(const httplib::Request& req, const char* name);

// body[key] as strings: empty if absent, nullopt if not an array of strings.
std::optional<std::vector<std::string>> StringList(const nlohmann::json& body, const char* key);

// A store ref ends up in a path ("<source>-<ref>" art) and on a store
// tool's command line, where a leading '-' would read as an option.
bool IsSafeRef(const std::string& ref);

Error GameRunningError(const std::string& id);
// One game's failure inside a batch reply: {id, error: {code, message, hint?, fix?}}.
nlohmann::json BatchFailure(const std::string& id, const Error& error);

}  // namespace mira::api
