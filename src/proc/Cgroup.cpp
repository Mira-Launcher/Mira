#include "proc/Cgroup.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <string>
#include <system_error>

namespace mira::proc::cgroup {

namespace {

constexpr const char* kRoot = "/sys/fs/cgroup";

Result<void> WriteControl(const std::filesystem::path& file, const std::string& text) {
  std::ofstream out(file);
  out << text;
  out.close();
  if (!out) return Err("cgroup_failed", std::format("couldn't write {}", file.string()));
  return {};
}

}  // namespace

std::optional<std::filesystem::path> Of(pid_t pid) {
  std::ifstream in(std::format("/proc/{}/cgroup", pid));
  std::string line;
  while (std::getline(in, line)) {
    if (!line.starts_with("0::")) continue;
    std::filesystem::path group = kRoot;
    group += line.substr(3);  // operator+= keeps the leading '/' of the cgroup path as a plain join
    return group;
  }
  return std::nullopt;
}

std::filesystem::path GameGroup(const std::filesystem::path& scope) {
  return scope / "game";
}

Result<void> Create(const std::filesystem::path& group) {
  std::error_code ec;
  std::filesystem::create_directory(group, ec);
  if (ec) return Err("cgroup_failed", std::format("couldn't create {}: {}", group.string(), ec.message()));
  return {};
}

std::vector<pid_t> Pids(const std::filesystem::path& group) {
  std::vector<pid_t> pids;
  std::ifstream in(group / "cgroup.procs");
  pid_t pid = 0;
  while (in >> pid) pids.push_back(pid);
  return pids;
}

bool Populated(const std::filesystem::path& group) {
  std::ifstream in(group / "cgroup.events");
  std::string line;
  while (std::getline(in, line)) {
    if (line == "populated 1") return true;
  }
  return false;
}

bool WaitEmpty(const std::filesystem::path& group, std::optional<std::chrono::milliseconds> timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout.value_or(std::chrono::milliseconds::zero());
  // A failed open leaves fd -1, which poll() ignores, so this degrades to a 1 s sleep.
  const int fd = ::open((group / "cgroup.events").c_str(), O_RDONLY | O_CLOEXEC);
  bool empty = !Populated(group);
  while (!empty) {
    int wait_ms = 1000;
    if (timeout) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
      if (left.count() <= 0) break;
      wait_ms = static_cast<int>(std::min<long long>(wait_ms, left.count()));
    }
    pollfd pfd{fd, POLLPRI, 0};
    ::poll(&pfd, 1, wait_ms);
    empty = !Populated(group);
  }
  if (fd >= 0) ::close(fd);
  return empty;
}

void Signal(const std::filesystem::path& group, int sig) {
  for (pid_t pid : Pids(group)) ::kill(pid, sig);
}

void Kill(const std::filesystem::path& group) {
  if (!WriteControl(group / "cgroup.kill", "1")) Signal(group, SIGKILL);
}

}  // namespace mira::proc::cgroup
