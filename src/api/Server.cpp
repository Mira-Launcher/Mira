#include "api/Server.h"

#include <sys/socket.h>

#include <filesystem>
#include <format>

#include <httplib.h>

#include "api/Http.h"
#include "api/Routes.h"
#include "core/Log.h"

namespace mira::api {

Server::Server(Services& services) : s_(services), http_(std::make_unique<httplib::Server>()) {}

Server::~Server() = default;

Result<void> Server::Serve(const std::filesystem::path& socket_path) {
  std::error_code ec;
  std::filesystem::create_directories(socket_path.parent_path(), ec);
  std::filesystem::remove(socket_path, ec);  // clear a stale socket from an unclean shutdown

  RegisterRoutes();

  // httplib's default is one thread per core with a floor of 8, and every
  // event stream and every long request (a scan, a move) holds one for its
  // whole length. Idle threads cost almost nothing.
  http_->new_task_queue = [] { return new httplib::ThreadPool(32); };
  http_->set_address_family(AF_UNIX);
  if (!http_->bind_to_port(socket_path.string(), 80)) {
    return Err("socket_bind_failed", std::format("cannot bind {}", socket_path.string()));
  }
  std::filesystem::permissions(socket_path, std::filesystem::perms::owner_read |
                                                std::filesystem::perms::owner_write,
                               ec);

  log::Info("listening on {}", socket_path.string());
  if (s_.stopping) return {};  // Stop() came before listening, where it would have done nothing
  s_.StartExternalWatch();
  if (!http_->listen_after_bind()) {
    return Err("socket_listen_failed", "httplib server exited unexpectedly");
  }
  return {};
}

void Server::Stop() {
  s_.BeginStopping();
  http_->stop();
}

void Server::RegisterRoutes() {
  // A body field of the wrong type throws out of a route; answer in the JSON envelope instead of a bare 500.
  http_->set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr error) {
    try {
      std::rethrow_exception(error);
    } catch (const nlohmann::json::exception& e) {
      SendError(res, 400, "invalid_body", e.what());
    } catch (const std::exception& e) {
      SendError(res, 500, "internal_error", e.what());
    }
  });
  // Unknown paths and wrong methods arrive with no body.
  http_->set_error_handler([](const httplib::Request&, httplib::Response& res) {
    if (!res.body.empty()) return;
    SendError(res, res.status, res.status == 405 ? "method_not_allowed" : "not_found",
              res.status == 405 ? "that method isn't supported here" : "no such endpoint");
  });
  RegisterConfigRoutes(*http_, s_);
  RegisterGameRoutes(*http_, s_);
  RegisterLaunchRoutes(*http_, s_);
  RegisterLibraryRoutes(*http_, s_);
  RegisterStoreRoutes(*http_, s_);
  RegisterLauncherRoutes(*http_, s_);
  RegisterMetadataRoutes(*http_, s_);
  RegisterRunnerRoutes(*http_, s_);
  RegisterEventRoutes(*http_, s_);
}

}  // namespace mira::api
