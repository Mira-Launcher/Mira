#include "support/LiveServer.h"

#include <doctest.h>
#include <sys/socket.h>

#include <chrono>

#include "support/TestEnv.h"

namespace mira::test {
namespace fs = std::filesystem;

LiveServer::LiveServer(const fs::path& state_dir)
    : config_(state_dir / "settings.toml"),
      games_(state_dir / "mira.db"),
      socket_path_(state_dir / "mirad.sock"),
      services_(config_, games_, events_),
      server_(services_) {
  config_.Load();
  Isolate(config_);
  games_.Load();
  thread_ = std::thread([this] { [[maybe_unused]] auto _ = server_.Serve(socket_path_); });
  // Serve() binds before it blocks accepting, but the thread needs a moment to start.
  for (int i = 0; i < 200 && !fs::exists(socket_path_); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

LiveServer::~LiveServer() {
  server_.Stop();
  if (thread_.joinable()) thread_.join();
}

httplib::Client LiveServer::Client() {
  httplib::Client client(socket_path_.string(), 80);
  client.set_address_family(AF_UNIX);
  return client;
}

nlohmann::json AwaitJob(httplib::Client& client, const httplib::Result& started) {
  REQUIRE(started != nullptr);
  REQUIRE(started->status == 202);
  const std::string id = nlohmann::json::parse(started->body).value("job", "");
  REQUIRE_FALSE(id.empty());
  for (int attempt = 0; attempt < 300; ++attempt) {
    auto job = client.Get("/v1/jobs/" + id);
    REQUIRE(job != nullptr);
    nlohmann::json state = nlohmann::json::parse(job->body);
    if (state.value("state", "") != "running") return state;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  FAIL("job never finished");
  return {};
}

std::optional<model::Event> WaitForEvent(const api::EventBus& events, std::string_view type) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (true) {
    for (const model::Event& event : events.Since(0)) {
      if (event.type == type) return event;
    }
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

}  // namespace mira::test
