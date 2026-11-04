#include "downloads/Aria2.h"

#include <httplib.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <format>
#include <random>
#include <thread>

#include "core/Log.h"
#include "runner/Exec.h"

namespace mira::downloads {
namespace {
using nlohmann::json;

std::string RandomHex() {
  std::random_device random;
  return std::format("{:08x}{:08x}{:08x}{:08x}", random(), random(), random(), random());
}

// A port nothing is using now. aria2 could lose it to another program before it binds, and then
// Ensure's wait fails and the next call tries again.
int FreePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t length = sizeof(address);
  int port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&address), length) == 0 &&
      ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
    port = ntohs(address.sin_port);
  }
  ::close(fd);
  return port;
}

}  // namespace

Aria2::~Aria2() {
  std::lock_guard lock(mutex_);
  Kill();
}

void Aria2::Kill() {
  if (pid_ <= 0) return;
  ::kill(pid_, SIGTERM);
  ::waitpid(pid_, nullptr, 0);
  pid_ = -1;
}

Result<void> Aria2::Ensure() {
  if (pid_ > 0) {
    int status = 0;
    if (::waitpid(pid_, &status, WNOHANG) == 0) return {};
    log::Warn("aria2 stopped unexpectedly");
    pid_ = -1;
  }
  const auto binary = runner::FindOnPath("aria2c");
  if (!binary) {
    return Err("aria2_missing", "Mira needs aria2 to download from links",
               "Install aria2 from your package manager.");
  }
  port_ = FreePort();
  if (port_ == 0) return Err("aria2_failed", "couldn't find a free local port for aria2");
  secret_ = RandomHex();
  Command command;
  command.argv = {*binary,
                  "--enable-rpc",
                  "--rpc-listen-all=false",
                  std::format("--rpc-listen-port={}", port_),
                  "--rpc-secret=" + secret_,
                  std::format("--stop-with-process={}", ::getpid()),
                  "--continue=true",
                  "--always-resume=false",  // a server that can't resume starts the file over
                  "--max-concurrent-downloads=100",
                  "--split=8",
                  "--max-connection-per-server=8",
                  "--min-split-size=20M",
                  "--max-tries=5",
                  "--retry-wait=10",
                  "--file-allocation=none",
                  "--auto-file-renaming=false",
                  "--content-disposition-default-utf8=true",
                  "--console-log-level=warn",
                  std::format("--max-overall-download-limit={}", limit_)};
  const auto started = runner::SpawnDetached(command);
  if (!started) return std::unexpected(started.error());
  pid_ = *started;
  httplib::Client client("127.0.0.1", port_);
  client.set_connection_timeout(std::chrono::milliseconds(200));
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (client.Get("/")) return {};  // any answer: it's listening
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  Kill();
  return Err("aria2_failed", "aria2 didn't start");
}

Result<json> Aria2::Call(const std::string& method, json params) {
  std::lock_guard lock(mutex_);
  if (auto ready = Ensure(); !ready) return std::unexpected(ready.error());
  json args = json::array({"token:" + secret_});
  for (json& param : params) args.push_back(std::move(param));
  const json body = {{"jsonrpc", "2.0"}, {"id", "mira"}, {"method", method}, {"params", args}};
  httplib::Client client("127.0.0.1", port_);
  client.set_read_timeout(std::chrono::seconds(5));
  const auto response = client.Post("/jsonrpc", body.dump(), "application/json");
  if (!response) return Err("aria2_failed", "couldn't reach aria2");
  const json answer = json::parse(response->body, nullptr, false);
  if (!answer.is_object()) return Err("aria2_failed", "aria2 gave an unreadable answer");
  if (answer.contains("error"))
    return Err("aria2_failed", answer["error"].value("message", std::string("aria2 failed")));
  return answer.value("result", json());
}

Result<std::string> Aria2::Add(const std::vector<std::string>& urls,
                               const std::filesystem::path& dir, const std::string& name) {
  json options = {{"dir", dir.string()}};
  if (!name.empty()) options["out"] = name;
  const auto gid = Call("aria2.addUri", json::array({urls, options}));
  if (!gid) return std::unexpected(gid.error());
  return gid->get<std::string>();
}

Result<void> Aria2::Pause(const std::string& gid) {
  const auto done = Call("aria2.forcePause", json::array({gid}));
  if (!done) return std::unexpected(done.error());
  return {};
}

Result<void> Aria2::Resume(const std::string& gid) {
  const auto done = Call("aria2.unpause", json::array({gid}));
  if (!done) return std::unexpected(done.error());
  return {};
}

void Aria2::Remove(const std::string& gid) {
  (void)Call("aria2.forceRemove", json::array({gid}));
}

Result<FileStatus> Aria2::Status(const std::string& gid) {
  const auto status = Call("aria2.tellStatus", json::array({gid}));
  if (!status) return std::unexpected(status.error());
  const auto number = [&status](const char* key) {
    return std::strtoll(status->value(key, std::string("0")).c_str(), nullptr, 10);
  };
  return FileStatus{.state = status->value("status", std::string()),
                    .total = number("totalLength"),
                    .done = number("completedLength"),
                    .speed = number("downloadSpeed"),
                    .error = status->value("errorMessage", std::string())};
}

Result<void> Aria2::SetSpeedLimit(std::int64_t bytes_per_second) {
  std::lock_guard lock(mutex_);
  limit_ = bytes_per_second;
  if (pid_ <= 0) return {};  // applied when it starts
  const auto done =
      Call("aria2.changeGlobalOption",
           json::array({json{{"max-overall-download-limit", std::to_string(bytes_per_second)}}}));
  if (!done) return std::unexpected(done.error());
  return {};
}

}  // namespace mira::downloads
