#pragma once

#include <sys/types.h>

#include <filesystem>
#include <optional>
#include <string>

#include "core/Command.h"
#include "core/Result.h"

// Named groups of processes Mira starts and later stops together: a systemd scope on Linux, and
// on macOS every process carrying the unit's name in its environment.
namespace mira::proc::units {

// Makes `command` start in `unit`.
void Wrap(Command& command, const std::string& unit, const std::string& description);
bool Active(const std::string& unit);
// Stops every unit whose name starts with `prefix`, without waiting.
void StopPrefix(const std::string& prefix);
// Stops `unit` and waits for it.
Result<void> Stop(const std::string& unit);

// A unit's processes, once the command started in it has joined.
class Scope {
 public:
  // nullopt until `pid` is in `unit`: on Linux systemd-run joins it just before exec'ing the
  // command.
  static std::optional<Scope> Of(pid_t pid, const std::string& unit);
  bool Populated() const;

 private:
#ifdef __linux__
  std::filesystem::path group_;
#else
  std::string entry_;
#endif
};

}  // namespace mira::proc::units
