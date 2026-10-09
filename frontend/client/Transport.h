#pragma once

#include <json.hpp>

#include <chrono>
#include <optional>
#include <string>

#include "ApiError.h"

// One request/response round trip against mirad's Unix socket.
//
// Owns the address family, the timeouts and the "2xx, or else unwrap the
// {"error": {...}} envelope" check, so no endpoint in client/api/ repeats them.
namespace mira_gui::transport {

struct Reply {
  bool ok = false;
  // 0 when the request never reached mirad. Carried for the endpoints where
  // a 404 is a normal answer rather than a failure: GET
  // /v1/games/{id}/metadata, for a game nothing has been fetched for.
  int status = 0;
  ApiError error;       // set when ok is false; the message is already human-readable
  nlohmann::json body;  // parsed response body, null if empty or unparsable
};

struct Options {
  // Left unset, httplib's own default applies. Set it for an endpoint that
  // does real work while the request is open: POST /v1/library/scan walks
  // every library root synchronously.
  std::optional<std::chrono::seconds> read_timeout;
};

// $MIRA_SOCKET if set, the same override `mirad --socket` accepts on the
// daemon side, so `MIRA_SOCKET=/path/to.sock mirad --socket /path/to.sock`
// and `MIRA_SOCKET=/path/to.sock mira-gui` unambiguously talk to each other
// regardless of what else is running. Without it, mirad's socket_path setting
// from settings.toml, else $XDG_RUNTIME_DIR/mira/mirad.sock, falling back to
// /tmp/mira if XDG_RUNTIME_DIR is unset. The CLI resolves it the same way.
std::string SocketPath();

Reply Get(const std::string& path, const Options& options = {});

// A response that isn't JSON: today only GET /v1/games/{id}/artwork, which
// answers with the image bytes themselves.
//
// `status` is carried out separately because 404 is a normal answer here,
// not a failure: it means this game has no cached artwork, which is the
// state most games are in. A caller that treats it as an error would
// produce one message per game on a fresh library.
struct Blob {
  bool ok = false;
  int status = 0;
  ApiError error;
  std::string bytes;
  std::string content_type;
};

Blob GetBinary(const std::string& path, const Options& options = {});
Reply Post(const std::string& path, const Options& options = {});
Reply PostJson(const std::string& path, const nlohmann::json& body, const Options& options = {});
Reply Patch(const std::string& path, const nlohmann::json& body, const Options& options = {});
Reply Put(const std::string& path, const std::string& body, const std::string& content_type,
          const Options& options = {});
Reply Delete(const std::string& path, const Options& options = {});

// The message for a reply that arrived intact but isn't the JSON kind the
// endpoint promises, a protocol mismatch rather than a transport failure,
// so it names the endpoint instead of the socket.
std::string UnexpectedResponse(const std::string& endpoint);

}  // namespace mira_gui::transport
