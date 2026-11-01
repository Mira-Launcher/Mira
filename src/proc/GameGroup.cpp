#include "proc/GameGroup.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <thread>

#ifdef __linux__
#include "proc/Cgroup.h"
#else
#include "proc/Procs.h"
#endif

namespace mira::proc {

std::string GroupEntry(const std::string& id) {
  return "MIRA_GROUP=" + id;
}

#ifdef __linux__

Result<GameGroup> GameGroup::Create() {
  const auto scope = cgroup::Of(::getpid());
  GameGroup group;
  if (scope) group.path_ = cgroup::GameGroup(*scope);
  if (!scope || !cgroup::Create(group.path_))
    return Err("no_cgroup", "game tracking needs a systemd user session (cgroup v2)");
  group.procs_ = (group.path_ / "cgroup.procs").string();
  return group;
}

std::optional<GameGroup> GameGroup::Of(pid_t wrapper) {
  const auto scope = cgroup::Of(wrapper);
  if (!scope) return std::nullopt;
  GameGroup group;
  group.path_ = cgroup::GameGroup(*scope);
  std::error_code ec;
  if (!std::filesystem::is_directory(group.path_, ec))
    return std::nullopt;  // not a mira-run scope (gone, or a reused pid)
  group.procs_ = (group.path_ / "cgroup.procs").string();
  return group;
}

std::vector<std::string> GameGroup::Env() const {
  return {};
}

bool GameGroup::JoinFromChild() const {
  const int fd = ::open(procs_.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool joined = ::write(fd, "0", 1) == 1;
  ::close(fd);
  return joined;
}

std::vector<pid_t> GameGroup::Pids() const {
  return cgroup::Pids(path_);
}
bool GameGroup::Populated() const {
  return cgroup::Populated(path_);
}
bool GameGroup::WaitEmpty(std::optional<std::chrono::milliseconds> timeout) const {
  return cgroup::WaitEmpty(path_, timeout);
}
void GameGroup::Signal(int sig) const {
  cgroup::Signal(path_, sig);
}
void GameGroup::Kill() const {
  cgroup::Kill(path_);
}

void GameGroup::Remove() const {
  std::error_code ec;
  std::filesystem::remove(path_, ec);  // an empty cgroup is a plain rmdir
}

#else

Result<GameGroup> GameGroup::Create() {
  GameGroup group;
  group.wrapper_ = ::getpid();
  return group;
}

std::optional<GameGroup> GameGroup::Of(pid_t wrapper) {
  if (::kill(wrapper, 0) != 0 || Name(wrapper) != "mira-run") return std::nullopt;
  GameGroup group;
  group.wrapper_ = wrapper;
  return group;
}

std::vector<std::string> GameGroup::Env() const {
  return {GroupEntry(std::to_string(wrapper_))};
}

bool GameGroup::JoinFromChild() const {
  return true;
}

std::vector<pid_t> GameGroup::Pids() const {
  std::vector<pid_t> pids = WithEntry(GroupEntry(std::to_string(wrapper_)));
  std::erase(pids, wrapper_);
  return pids;
}

bool GameGroup::Populated() const {
  return !Pids().empty();
}

// Polled: nothing tells when a process outside our own children exits.
bool GameGroup::WaitEmpty(std::optional<std::chrono::milliseconds> timeout) const {
  const auto deadline =
      std::chrono::steady_clock::now() + timeout.value_or(std::chrono::milliseconds::zero());
  while (Populated()) {
    if (timeout && std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  return true;
}

void GameGroup::Signal(int sig) const {
  for (const pid_t pid : Pids()) ::kill(pid, sig);
}

// Twice, for what a dying process forked in between.
void GameGroup::Kill() const {
  Signal(SIGKILL);
  Signal(SIGKILL);
}

void GameGroup::Remove() const {}

#endif

}  // namespace mira::proc
