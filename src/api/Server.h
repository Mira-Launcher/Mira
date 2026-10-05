#pragma once

#include <filesystem>
#include <memory>

#include "api/Services.h"
#include "core/Result.h"

// httplib::Server is used only by the route files; forward-declared here so
// including this header does not pull the whole vendored library into every
// translation unit that wires up a daemon.
namespace httplib {
class Server;
}

namespace mira::api {

// The REST-over-UDS surface described in docs/api.md. Owns the socket and the
// httplib server; everything the routes act on lives in Services, so they can
// be exercised without a real socket in tests.
class Server {
public:
  explicit Server(Services& services);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds the UDS socket (removing a stale one first) and blocks serving
  // until Stop() is called from another thread. The bind happens inside this
  // call so a failure is reported through the return value rather than a
  // separate two-step API.
  Result<void> Serve(const std::filesystem::path& socket_path);
  void Stop();

private:
  void RegisterRoutes();

  Services& s_;
  std::unique_ptr<httplib::Server> http_;
};

}  // namespace mira::api
