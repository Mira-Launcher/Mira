#include "itch/Butlerd.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/Json.h"
#include "core/Log.h"
#include "core/StoreErrors.h"
#include "itch/Itch.h"

namespace mira::itch {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

struct Pending {
  std::optional<Result<json>> reply;
  NotificationHandler on_notification;
};

// Every call shares one connection: calls are sent concurrently and a reader
// thread hands each reply back by id. butlerd runs with --keep-alive so it
// would take more connections, and --destiny-pid so it exits with mirad.
struct Connection {
  std::mutex connect_mutex;  // one startup at a time; held without `mutex`, so calls on a live connection never wait on it
  std::mutex mutex;  // everything below except writes
  std::condition_variable replied;
  bool connected = false;
  int socket_fd = -1;
  pid_t daemon_pid = -1;
  int next_id = 1;
  std::map<int, Pending*> pending;
  std::mutex write_mutex;  // one message on the socket at a time
};

// Kills and reaps a butlerd that is no longer usable, so a retry doesn't
// leave it running beside the next one on the same butler.db.
void StopDaemon(pid_t& pid) {
  if (pid <= 0) return;
  kill(-pid, SIGKILL);  // its own session and group (setsid)
  kill(pid, SIGKILL);
  waitpid(pid, nullptr, 0);
  pid = -1;
}

Connection& GlobalConnection() {
  static Connection connection;
  return connection;
}

// Forks `argv`, redirecting only the child's stdout to a pipe (stderr goes
// to mirad's own, same posture as runner::SpawnDetached) -- the caller
// gets that pipe's read end and owns closing it, and the pid to stop it. Not built on runner::SpawnDetached*,
// which don't offer a way to read a long-lived child's stdout while it
// keeps running -- this is the one place in this codebase that needs
// that shape (see Butlerd.h's class comment).
struct Spawned {
  pid_t pid;
  int stdout_fd;
};

Result<Spawned> SpawnCapturingStdout(const std::vector<std::string>& argv) {
  // Built before fork(): the child of a threaded process mustn't allocate.
  std::vector<char*> args;
  args.reserve(argv.size() + 1);
  for (const std::string& arg : argv) args.push_back(const_cast<char*>(arg.c_str()));
  args.push_back(nullptr);

  // CLOEXEC so children other threads spawn meanwhile don't hold the write end.
  int pipe_fds[2];
  if (pipe2(pipe_fds, O_CLOEXEC) != 0) return Err("pipe_failed", std::strerror(errno));

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return Err("fork_failed", std::strerror(errno));
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    dup2(pipe_fds[1], STDOUT_FILENO);
    close(pipe_fds[1]);
    setsid();
    sigset_t none;  // mirad blocks SIGINT/SIGTERM; the mask would survive exec
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    execvp(args[0], args.data());
    _exit(127);
  }
  close(pipe_fds[1]);
  return Spawned{pid, pipe_fds[0]};
}

// Reads lines off `fd`, a block at a time. poll() enforces the deadline; a bare read() would block forever
// on a reply that never comes. Bytes past a line stay buffered for the next call.
class LineReader {
public:
  explicit LineReader(int fd) : fd_(fd) {}

  Result<std::string> Read(std::chrono::steady_clock::time_point deadline) {
    while (true) {
      if (const size_t newline = buffer_.find('\n'); newline != std::string::npos) {
        std::string line = buffer_.substr(0, newline);
        buffer_.erase(0, newline + 1);
        return line;
      }
      const auto left =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0) return Err("timeout", "no response within the deadline");
      pollfd ready{.fd = fd_, .events = POLLIN, .revents = 0};
      const int polled = poll(&ready, 1, static_cast<int>(std::min<std::int64_t>(left.count(), 60'000)));
      if (polled < 0 && errno != EINTR) return Err("read_failed", std::strerror(errno));
      if (polled <= 0) continue;
      char chunk[4096];
      const ssize_t n = read(fd_, chunk, sizeof(chunk));
      if (n < 0) {
        if (errno == EINTR) continue;
        return Err("read_failed", std::strerror(errno));
      }
      if (n == 0) return Err("eof", "connection closed before a full line arrived");
      buffer_.append(chunk, static_cast<size_t>(n));
    }
  }

private:
  int fd_;
  std::string buffer_;
};

Result<void> ConnectSocket(int& out_fd, const std::string& host, int port) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0 || result == nullptr) {
    return Err("resolve_failed", "couldn't resolve butlerd's own listen address " + host);
  }
  const int fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  if (fd < 0) {
    freeaddrinfo(result);
    return Err("socket_failed", std::strerror(errno));
  }
  if (connect(fd, result->ai_addr, result->ai_addrlen) != 0) {
    freeaddrinfo(result);
    close(fd);
    return Err("connect_failed", std::strerror(errno));
  }
  freeaddrinfo(result);
  out_fd = fd;
  return {};
}

Result<void> SendLine(int fd, const json& message) {
  const std::string line = message.dump() + "\n";
  size_t sent = 0;
  while (sent < line.size()) {
    const ssize_t n = write(fd, line.data() + sent, line.size() - sent);
    if (n < 0) {
      if (errno == EINTR) continue;
      return Err("write_failed", std::strerror(errno));
    }
    sent += static_cast<size_t>(n);
  }
  return {};
}

// The index of the upload to install: a Linux build first, then Windows,
// then whatever butlerd listed first.
int PickUpload(const json& uploads) {
  if (!uploads.is_array() || uploads.empty()) return 0;
  for (const char* platform : {"linux", "windows"}) {
    for (size_t i = 0; i < uploads.size(); ++i) {
      const json platforms = uploads[i].value("platforms", json::object());
      if (platforms.is_object() && platforms.contains(platform)) return static_cast<int>(i);
    }
  }
  return 0;
}

// butlerd asks the client things mid-call (which upload, accept a licence)
// and waits for the answer, so every request gets one.
void AnswerServerRequest(int fd, const json& request, std::mutex* write_mutex = nullptr) {
  const std::string method = request.value("method", std::string());
  const json params = request.value("params", json::object());
  json reply = {{"jsonrpc", "2.0"}, {"id", request["id"]}};
  if (method == "PickUpload") {
    reply["result"] = {{"index", PickUpload(params.value("uploads", json::array()))}};
  } else if (method == "AcceptLicense") {
    reply["result"] = {{"accept", true}};
  } else if (method == "ExternalUploadsAreBad") {
    reply["result"] = {{"whatever", true}};
  } else {
    log::Warn("butlerd asked something Mira doesn't answer: {}", method);
    reply["error"] = {{"code", -32601}, {"message", "not handled by Mira: " + method}};
  }
  if (write_mutex == nullptr) {
    (void)SendLine(fd, reply);
    return;
  }
  const std::lock_guard<std::mutex> lock(*write_mutex);
  (void)SendLine(fd, reply);
}

// Runs for the life of one butlerd connection: replies go to their pending
// call, notifications to every call that wants them, and butlerd's own
// requests get answered. When the connection drops, every waiting call fails.
void ReadLoop(Connection& connection, int fd, LineReader reader) {
  const auto forever = std::chrono::steady_clock::now() + std::chrono::hours(24 * 365);
  // A throw (odd JSON, a handler) ends up as a disconnect, so waiting callers still fail instead of the daemon terminating.
  try {
  while (true) {
    const Result<std::string> line = reader.Read(forever);
    if (!line) break;
    const json parsed = json::parse(*line, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) continue;
    if (parsed.contains("method")) {
      if (parsed.contains("id")) {
        AnswerServerRequest(fd, parsed, &connection.write_mutex);
        continue;
      }
      std::vector<NotificationHandler> handlers;
      {
        const std::lock_guard<std::mutex> lock(connection.mutex);
        for (const auto& [id, call] : connection.pending) {
          if (call->on_notification) handlers.push_back(call->on_notification);
        }
      }
      const std::string method = core::JsonString(parsed, "method");
      const json params = parsed.contains("params") ? parsed["params"] : json::object();
      for (const NotificationHandler& handler : handlers) handler(method, params);
      continue;
    }
    if (!parsed.contains("id") || !parsed["id"].is_number_integer()) continue;
    const std::lock_guard<std::mutex> lock(connection.mutex);
    const auto found = connection.pending.find(parsed["id"].get<int>());
    if (found == connection.pending.end()) continue;  // its caller gave up
    if (parsed.contains("error")) {
      found->second->reply = Err("butlerd_error", core::JsonString(parsed["error"], "message", "butlerd call failed"));
    } else {
      found->second->reply = parsed.value("result", json::object());
    }
    connection.replied.notify_all();
  }
  } catch (const std::exception& error) {
    log::Warn("butlerd reader stopped: {}", error.what());
  }

  const std::lock_guard<std::mutex> lock(connection.mutex);
  if (connection.socket_fd == fd) {
    connection.connected = false;
    connection.socket_fd = -1;
    StopDaemon(connection.daemon_pid);
  }
  close(fd);
  for (const auto& [id, call] : connection.pending) {
    if (!call->reply) call->reply = Err("butlerd_disconnected", "lost the connection to butlerd");
  }
  connection.replied.notify_all();
}

// Reads lines off the connection until one carries the response to
// `request_id` (a "result"/"error" field alongside a matching "id").
// Server requests are answered; notifications (e.g. install progress) go
// to `on_notification`, if set.
Result<json> ReadResponse(int fd, LineReader& reader, int request_id,
                          std::chrono::steady_clock::time_point deadline,
                          const NotificationHandler& on_notification = nullptr) {
  while (true) {
    const Result<std::string> line = reader.Read(deadline);
    if (!line) return std::unexpected(line.error());
    const json parsed = json::parse(*line, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) continue;
    if (parsed.contains("method")) {
      if (parsed.contains("id")) {
        AnswerServerRequest(fd, parsed);
      } else if (on_notification) {
        on_notification(parsed.value("method", std::string()), parsed.value("params", json::object()));
      }
      continue;
    }
    if (parsed.contains("id") && parsed["id"].is_number_integer() &&
        parsed["id"].get<int>() == request_id) {
      if (parsed.contains("error")) {
        return Err("butlerd_error", parsed["error"].value("message", std::string("butlerd call failed")));
      }
      return parsed.value("result", json::object());
    }
    // A notification for some other in-flight call, or one we don't act
    // on -- move on.
  }
}

// Starts butler (if not already connected) and performs Meta.Authenticate. Waits on butlerd holding only
// `connect_mutex`, so calls on a live connection are never held up by a startup.
Result<void> EnsureConnected(const config::Config& config, Connection& connection) {
  const std::lock_guard connect_lock(connection.connect_mutex);
  {
    const std::lock_guard lock(connection.mutex);
    if (connection.connected) return {};
  }

  const runner::ToolStatus status = DetectButler(config);
  if (!status.installed) {
    return StoreToolMissing("itch", "itch.io", "butler");
  }

  const fs::path db_path = config.File().parent_path() / "tools" / "itch" / "butler.db";
  std::error_code ec;
  fs::create_directories(db_path.parent_path(), ec);

  const Result<Spawned> spawned =
    SpawnCapturingStdout({status.path, "daemon", "--json", "--dbpath", db_path.string(), "--keep-alive",
                          "--destiny-pid", std::to_string(getpid())});
  if (!spawned) return std::unexpected(spawned.error());
  pid_t pid = spawned->pid;
  const int stdout_fd = spawned->stdout_fd;

  // butlerd mixes its own JSON log lines in with the one
  // butlerd/listen-notification line we need; read until we see it or run
  // out of time.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::string secret;
  std::string address;
  LineReader stdout_reader(stdout_fd);
  while (secret.empty()) {
    const Result<std::string> line = stdout_reader.Read(deadline);
    if (!line) {
      close(stdout_fd);
      StopDaemon(pid);
      return Err("butlerd_start_failed", "butlerd didn't print its listen address in time: " + line.error().message);
    }
    const json parsed = json::parse(*line, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) continue;
    if (parsed.value("type", std::string()) != "butlerd/listen-notification") continue;
    secret = parsed.value("secret", std::string());
    address = parsed.value("tcp", json::object()).value("address", std::string());
  }

  // butlerd keeps logging to its stdout for as long as it runs -- drain it
  // in the background forever so the child never blocks writing to a full
  // pipe once we stop reading it ourselves.
  const int drain_fd = stdout_fd;
  std::thread([drain_fd] {
    char buffer[4096];
    while (read(drain_fd, buffer, sizeof(buffer)) > 0) {
    }
    close(drain_fd);
  }).detach();

  const size_t colon = address.rfind(':');
  if (colon == std::string::npos) {
    StopDaemon(pid);
    return Err("butlerd_start_failed", "unexpected listen address: " + address);
  }
  const std::string host = address.substr(0, colon);
  const int port = std::atoi(address.substr(colon + 1).c_str());

  int socket_fd = -1;
  if (auto connected = ConnectSocket(socket_fd, host, port); !connected) {
    StopDaemon(pid);
    return std::unexpected(connected.error());
  }

  int auth_id = 0;
  {
    const std::lock_guard lock(connection.mutex);
    auth_id = connection.next_id++;
  }
  if (auto sent = SendLine(socket_fd, {{"jsonrpc", "2.0"}, {"id", auth_id}, {"method", "Meta.Authenticate"},
                                       {"params", {{"secret", secret}}}});
      !sent) {
    close(socket_fd);
    StopDaemon(pid);
    return std::unexpected(sent.error());
  }
  LineReader socket_reader(socket_fd);
  const Result<json> auth_result = ReadResponse(socket_fd, socket_reader, auth_id, deadline);
  if (!auth_result) {
    close(socket_fd);
    StopDaemon(pid);
    return std::unexpected(auth_result.error());
  }

  {
    const std::lock_guard lock(connection.mutex);
    connection.socket_fd = socket_fd;
    connection.daemon_pid = pid;
    connection.connected = true;
  }
  std::thread(ReadLoop, std::ref(connection), socket_fd, std::move(socket_reader)).detach();
  log::Info("connected to butlerd at {}", address);
  return {};
}

}  // namespace

namespace {

Result<json> Send(const config::Config& config, const std::string& method, const json& params,
                  std::chrono::steady_clock::duration timeout, const NotificationHandler& on_notification) {
  Connection& connection = GlobalConnection();
  Pending call{.reply = std::nullopt, .on_notification = on_notification};
  int id = 0;
  int fd = -1;
  // Twice: the connection can drop between connecting and registering the call.
  for (int attempt = 0; attempt < 2 && fd < 0; ++attempt) {
    if (auto ready = EnsureConnected(config, connection); !ready) return std::unexpected(ready.error());
    const std::lock_guard<std::mutex> lock(connection.mutex);
    if (!connection.connected) continue;
    id = connection.next_id++;
    fd = connection.socket_fd;
    connection.pending[id] = &call;
  }
  if (fd < 0) return Err("butlerd_disconnected", "lost the connection to butlerd");
  Result<void> sent;
  {
    const std::lock_guard<std::mutex> lock(connection.write_mutex);
    sent = SendLine(fd, {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
  }
  std::unique_lock<std::mutex> lock(connection.mutex);
  if (!sent) {
    connection.pending.erase(id);
    return std::unexpected(sent.error());
  }
  const bool answered = connection.replied.wait_for(lock, timeout, [&call] { return call.reply.has_value(); });
  connection.pending.erase(id);
  if (!answered) return Err("timeout", "butlerd didn't answer " + method + " in time");
  return *call.reply;
}

}  // namespace

Result<json> Call(const config::Config& config, const std::string& method, const json& params) {
  return Send(config, method, params, std::chrono::seconds(120), nullptr);
}

Result<json> CallLong(const config::Config& config, const std::string& method, const json& params,
                      const NotificationHandler& on_notification) {
  // A download can take hours; the limit only guards against a hang.
  return Send(config, method, params, std::chrono::hours(24), on_notification);
}

}  // namespace mira::itch
