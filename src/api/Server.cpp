#include "api/Server.h"

#include <sys/socket.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <regex>

#include <httplib.h>

#include "api/Http.h"
#include "api/Routes.h"
#include "core/Lane.h"
#include "core/Log.h"

namespace mira::api {
namespace {

// httplib's ThreadPool starts every thread at once and keeps them; a Lane starts them as requests come and lets
// idle ones go.
class LaneQueue final : public httplib::TaskQueue {
public:
  bool enqueue(std::function<void()> fn) override {
    lane_.Post(std::move(fn));
    return true;
  }
  void shutdown() override { lane_.Stop(); }

private:
  Lane lane_{"http", 64};
};

using nlohmann::json;

// Whether a request names a game or store title that locked hidden games conceal: in its path
// (/v1/games/{id}), its query (source and ref) or its body (ids, id, source with ref, refs or
// titles).
bool NamesConcealed(Services& s, const httplib::Request& req) {
  static const std::regex game_path(R"(^/v1/games/([^/]+))");
  if (std::smatch match;
      std::regex_search(req.path, match, game_path) && s.Conceals(match[1].str()))
    return true;
  if (req.has_param("source") && req.has_param("ref") &&
      s.Conceals(req.get_param_value("source") + "-" + req.get_param_value("ref"))) {
    return true;
  }
  if (req.body.empty()) return false;
  const json body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
  if (!body.is_object()) return false;
  const auto names = [&](const json& value, const std::string& prefix) {
    return value.is_string() && s.Conceals(prefix + value.get<std::string>());
  };
  if (body.contains("id") && names(body["id"], "")) return true;
  if (body.contains("ids") && body["ids"].is_array() &&
      std::ranges::any_of(body["ids"], [&](const json& id) { return names(id, ""); })) {
    return true;
  }
  if (!body.contains("source") || !body["source"].is_string()) return false;
  const std::string prefix = body["source"].get<std::string>() + "-";
  if (body.contains("ref") && names(body["ref"], prefix)) return true;
  if (body.contains("refs") && body["refs"].is_array() &&
      std::ranges::any_of(body["refs"], [&](const json& ref) { return names(ref, prefix); })) {
    return true;
  }
  return body.contains("titles") && body["titles"].is_array() &&
         std::ranges::any_of(body["titles"], [&](const json& title) {
           return title.is_object() && title.contains("ref") && names(title["ref"], prefix);
         });
}

}  // namespace

Server::Server(Services& services) : s_(services), http_(std::make_unique<httplib::Server>()) {}

Server::~Server() = default;

Result<void> Server::Serve(const std::filesystem::path& socket_path) {
  std::error_code ec;
  std::filesystem::create_directories(socket_path.parent_path(), ec);
  std::filesystem::remove(socket_path, ec);  // clear a stale socket from an unclean shutdown

  RegisterRoutes();

  // Every event stream and every long request (a scan, a move) holds a thread for its whole length.
  http_->new_task_queue = [] { return new LaneQueue; };
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
  // While hidden games are locked, a request naming one is answered as if it weren't there.
  Router router(*http_, [this](const httplib::Request& req, httplib::Response& res) {
    if (!s_.hidden.Locked()) return false;
    if (NamesConcealed(s_, req)) {
      SendError(res, 404, "game_not_found", "no such game");
      return true;
    }
    static const std::regex job_path(R"(^/v1/jobs/([^/]+))");
    if (std::smatch match; std::regex_search(req.path, match, job_path)) {
      const auto job = s_.jobs.Find(match[1].str());
      if (job && s_.Conceals(job->value("target", std::string()))) {
        SendError(res, 404, "job_not_found", "no such job, or it finished too long ago");
        return true;
      }
    }
    return false;
  });
  RegisterConfigRoutes(router, s_);
  RegisterGameRoutes(router, s_);
  RegisterLaunchRoutes(router, s_);
  RegisterLibraryRoutes(router, s_);
  RegisterTagsRoutes(router, s_);
  RegisterHiddenRoutes(router, s_);
  RegisterStoreRoutes(router, s_);
  RegisterLauncherRoutes(router, s_);
  RegisterEmulatorRoutes(router, s_);
  RegisterModManagerRoutes(router, s_);
  RegisterUpdateRoutes(router, s_);
  RegisterMetadataRoutes(router, s_);
  RegisterRunnerRoutes(router, s_);
  RegisterEventRoutes(router, s_);
  RegisterLogRoutes(router, s_);
}

Router::Handler Router::Gated(Handler handler) const {
  return [gate = gate_, handler = std::move(handler)](const httplib::Request& req,
                                                      httplib::Response& res) {
    if (!gate(req, res)) handler(req, res);
  };
}

void Router::Get(const std::string& pattern, Handler handler) {
  http_.Get(pattern, Gated(std::move(handler)));
}
void Router::Post(const std::string& pattern, Handler handler) {
  http_.Post(pattern, Gated(std::move(handler)));
}
void Router::Put(const std::string& pattern, Handler handler) {
  http_.Put(pattern, Gated(std::move(handler)));
}
void Router::Patch(const std::string& pattern, Handler handler) {
  http_.Patch(pattern, Gated(std::move(handler)));
}
void Router::Delete(const std::string& pattern, Handler handler) {
  http_.Delete(pattern, Gated(std::move(handler)));
}

}  // namespace mira::api
