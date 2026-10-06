#include "core/Log.h"

#include "core/LogHub.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

namespace mira::log {
namespace {

std::atomic<Level> g_level{Level::Info};
std::mutex g_mutex;

constexpr std::string_view Name(Level level) {
  switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info:  return "INFO";
    case Level::Warn:  return "WARN";
    case Level::Error: return "ERROR";
  }
  return "?";
}

}  // namespace

void SetLevel(Level level) { g_level.store(level, std::memory_order_relaxed); }

void Write(Level level, std::string_view message) {
  if (level < g_level.load(std::memory_order_relaxed)) return;
  // journald captures stderr and adds its own timestamps, so we add none.
  loghub::Append("daemon", std::string(Name(level)) + " " + std::string(message) + "\n");
  std::lock_guard lock(g_mutex);
  std::fprintf(stderr, "%-5.*s %.*s\n", static_cast<int>(Name(level).size()), Name(level).data(),
               static_cast<int>(message.size()), message.data());
}

}  // namespace mira::log
