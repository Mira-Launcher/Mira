#pragma once

#include <httplib.h>

#include <filesystem>
#include <optional>
#include <string_view>
#include <thread>

#include <json.hpp>

#include "api/EventBus.h"
#include "api/Server.h"
#include "api/Services.h"
#include "config/Config.h"
#include "store/GameStore.h"

namespace mira::test {

// A real Server on a real UDS socket for the lifetime of a test, with an
// isolated config (see Isolate), so a test drives mirad the way a client does.
class LiveServer {
 public:
  explicit LiveServer(const std::filesystem::path& state_dir);
  ~LiveServer();

  httplib::Client Client();

  store::GameStore& games() { return games_; }
  api::EventBus& events() { return events_; }
  const std::filesystem::path& socket_path() const { return socket_path_; }
  const config::Config& config() const { return config_; }
  config::Config& MutableConfig() { return config_; }
  api::Services& services() { return services_; }

 private:
  config::Config config_;
  store::GameStore games_;
  api::EventBus events_;
  std::filesystem::path socket_path_;
  api::Services services_;
  api::Server server_;
  std::thread thread_;
};

// A long request answers 202 {job}; this waits for the job and returns it
// as GET /v1/jobs/{id} shows it once done (`state`, then `result` or `error`).
nlohmann::json AwaitJob(httplib::Client& client, const httplib::Result& started);

// The first event of `type` published so far or within 10 s; nullopt if none came.
std::optional<model::Event> WaitForEvent(const api::EventBus& events, std::string_view type);

}  // namespace mira::test
