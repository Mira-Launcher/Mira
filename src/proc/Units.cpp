#include "proc/Units.h"

#include <signal.h>

#include <chrono>
#include <thread>

#include "core/Log.h"
#include "proc/Procs.h"
#include "runner/Exec.h"

namespace mira::proc::units {

#ifdef __linux__

void Wrap(Command& command, const std::string& unit, const std::string& description) {
  command.argv.insert(command.argv.begin(),
                      {"systemd-run", "--user", "--scope", "--quiet", "--collect", "--unit", unit,
                       "--description", description, "--"});
}

bool Active(const std::string& unit) {
  Command active;
  active.argv = {"systemctl", "--user", "is-active", "--quiet", unit};
  const auto ran = runner::RunAndWait(active);
  return ran && ran->exit_code == 0;
}

void StopPrefix(const std::string& prefix) {
  Command command;
  command.argv = {"systemctl", "--user", "stop", "--no-block", prefix + "*"};
  if (auto spawned = runner::SpawnReaped(command); !spawned)
    log::Warn("couldn't stop {}*: {}", prefix, spawned.error().message);
}

Result<void> Stop(const std::string& unit) {
  Command command;
  command.argv = {"systemctl", "--user", "stop", unit};
  command.timeout_s = 15;
  const auto ran = runner::RunAndWait(command);
  if (!ran) return std::unexpected(ran.error());
  return {};
}

#else

namespace {

constexpr std::string_view kKey = "MIRA_UNIT=";

std::vector<pid_t> WithPrefix(const std::string& prefix) {
  const std::string start = std::string(kKey) + prefix;
  std::vector<pid_t> found;
  for (const pid_t pid : AllPids()) {
    const auto environ = Environ(pid);
    if (!environ) continue;
    for (size_t at = 0; at < environ->size();) {
      const size_t end = std::min(environ->find('\0', at), environ->size());
      if (environ->compare(at, start.size(), start) == 0 && end - at >= start.size()) {
        found.push_back(pid);
        break;
      }
      at = end + 1;
    }
  }
  return found;
}

}  // namespace

void Wrap(Command& command, const std::string& unit, const std::string&) {
  command.env["MIRA_UNIT"] = unit;
}

bool Active(const std::string& unit) {
  return !WithEntry(std::string(kKey) + unit).empty();
}

void StopPrefix(const std::string& prefix) {
  for (const pid_t pid : WithPrefix(prefix)) ::kill(pid, SIGTERM);
}

// SIGTERM, then SIGKILL for what's left after 15 s, as systemd does.
Result<void> Stop(const std::string& unit) {
  const std::string entry = std::string(kKey) + unit;
  for (const pid_t pid : WithEntry(entry)) ::kill(pid, SIGTERM);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!WithEntry(entry).empty()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      for (const pid_t pid : WithEntry(entry)) ::kill(pid, SIGKILL);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return {};
}

#endif

}  // namespace mira::proc::units
