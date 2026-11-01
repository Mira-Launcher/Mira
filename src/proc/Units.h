#pragma once

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

}  // namespace mira::proc::units
