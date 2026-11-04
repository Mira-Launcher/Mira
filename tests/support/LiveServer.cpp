#include "support/LiveServer.h"

#include <doctest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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
  // The socket file appears at bind(), before listen(), so wait until a connection is taken.
  const bool listening = WaitUntil([&] {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    socket_path_.string().copy(addr.sun_path, sizeof(addr.sun_path) - 1);
    const bool connected = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return connected;
  });
  if (!listening) {
    server_.Stop();
    thread_.join();
    FAIL("mirad never listened on " << socket_path_);
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
  // Generous: only a broken job waits it out, and an ASan build on a busy runner is slow.
  nlohmann::json state;
  const bool done = WaitUntil(
      [&] {
        auto job = client.Get("/v1/jobs/" + id);
        REQUIRE(job != nullptr);
        state = nlohmann::json::parse(job->body);
        return state.value("state", "") != "running";
      },
      std::chrono::seconds(60));
  if (!done) FAIL("job never finished: " << state.dump());
  return state;
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
