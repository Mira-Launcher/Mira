#include "Transport.h"

#include <httplib.h>
#include <pwd.h>
#include <toml.hpp>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>

#include "JsonMapping.h"

namespace mira_gui::transport {
namespace {

using nlohmann::json;

httplib::Client MakeClient(const Options& options) {
  httplib::Client client(SocketPath(), 80);
  client.set_address_family(AF_UNIX);
  client.set_connection_timeout(std::chrono::seconds(2));
  if (options.read_timeout) client.set_read_timeout(*options.read_timeout);
  return client;
}

ApiError Unreachable(const httplib::Result& res) {
  return ApiError("cannot reach mirad at " + SocketPath() + " (" + httplib::to_string(res.error()) + ")",
                  ApiError::kUnreachable);
}

// mirad's {"error": {...}} envelope, or the raw body if it isn't one. Never JSON shown as a message.
ApiError FromBody(int status, const std::string& body_text) {
  const json body = json::parse(body_text, nullptr, false);
  if (body.is_discarded() || !body.contains("error") || !body["error"].is_object()) {
    return ApiError(body_text.empty() || !body.is_discarded() ? "mirad answered HTTP " + std::to_string(status)
                                                               : body_text);
  }
  ApiError error = mapping::ToApiError(body["error"]);
  if (error.message.empty()) error.message = error.code.empty() ? "mirad answered HTTP " + std::to_string(status) : error.code;
  return error;
}

// An httplib::Result carries its own error when the request never got a
// response at all; when it did, mirad's error body is the envelope above.
Reply Finish(const httplib::Result& res) {
  Reply reply;
  if (res) reply.status = res->status;
  if (res && res->status >= 200 && res->status < 300) {
    reply.ok = true;
    reply.body = json::parse(res->body, nullptr, false);
    if (reply.body.is_discarded()) reply.body = json();
    return reply;
  }

  reply.error = res ? FromBody(res->status, res->body) : Unreachable(res);
  return reply;
}

// paths::Home in src/core/Paths.cpp, which the frontend can't link.
std::string Home() {
  if (const char* home = std::getenv("HOME"); home && *home) return home;
  if (const passwd* pw = ::getpwuid(::getuid())) return pw->pw_dir;
  return "/tmp";
}

// mirad's socket_path setting from settings.toml, expanded as paths::Expand does (keep the two
// in step), or "" when it isn't set there. Read once: mirad itself only reads it at startup.
std::string ConfiguredSocket() {
  const char* config_home = std::getenv("XDG_CONFIG_HOME");
  const std::filesystem::path base = config_home && *config_home
                                         ? std::filesystem::path(config_home)
                                         : std::filesystem::path(Home()) / ".config";
  toml::parse_result parsed = toml::parse_file((base / "mira" / "settings.toml").string());
  if (!parsed) return {};
  const std::optional<std::string> raw = parsed.table()["socket_path"].value<std::string>();
  if (!raw || raw->empty()) return {};

  std::string out;
  for (size_t i = 0; i < raw->size(); ++i) {
    const char c = (*raw)[i];
    if (c == '~' && i == 0 && (raw->size() == 1 || (*raw)[1] == '/')) {
      out += Home();
    } else if (c == '$' && i + 1 < raw->size()) {
      size_t end = i + 1;
      while (end < raw->size() && (std::isalnum(static_cast<unsigned char>((*raw)[end])) || (*raw)[end] == '_')) ++end;
      if (const char* value = std::getenv(raw->substr(i + 1, end - i - 1).c_str())) out += value;
      i = end - 1;
    } else {
      out += c;
    }
  }
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  return out;
}

}  // namespace

std::string SocketPath() {
  const char* override_path = std::getenv("MIRA_SOCKET");
  if (override_path && *override_path) return override_path;

  static const std::string configured = ConfiguredSocket();
  if (!configured.empty()) return configured;

  const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
  const std::filesystem::path base = runtime_dir && *runtime_dir ? runtime_dir : "/tmp";
  return (base / "mira" / "mirad.sock").string();
}

Reply Get(const std::string& path, const Options& options) {
  return Finish(MakeClient(options).Get(path));
}

Blob GetBinary(const std::string& path, const Options& options) {
  const httplib::Result res = MakeClient(options).Get(path);
  Blob blob;
  if (!res) {
    blob.error = Unreachable(res);
    return blob;
  }

  blob.status = res->status;
  if (res->status < 200 || res->status >= 300) {
    blob.error = FromBody(res->status, res->body);
    return blob;
  }

  blob.ok = true;
  blob.bytes = res->body;
  blob.content_type = res->get_header_value("Content-Type");
  return blob;
}

Reply Post(const std::string& path, const Options& options) {
  return Finish(MakeClient(options).Post(path));
}

Reply PostJson(const std::string& path, const json& body, const Options& options) {
  return Finish(MakeClient(options).Post(path, body.dump(), "application/json"));
}

Reply Patch(const std::string& path, const json& body, const Options& options) {
  return Finish(MakeClient(options).Patch(path, body.dump(), "application/json"));
}

Reply Delete(const std::string& path, const Options& options) {
  return Finish(MakeClient(options).Delete(path));
}

std::string UnexpectedResponse(const std::string& endpoint) {
  return "mirad returned an unexpected response for " + endpoint;
}

}  // namespace mira_gui::transport
