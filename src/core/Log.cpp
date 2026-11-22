#include "core/Log.h"

#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

#include "core/LogHub.h"

namespace mira::log {
namespace {

std::atomic<Level> g_level{Level::Info};
std::mutex g_mutex;
std::ofstream g_file;  // open after SetFile

constexpr std::string_view Name(Level level) {
  switch (level) {
    case Level::Debug:
      return "DEBUG";
    case Level::Info:
      return "INFO";
    case Level::Warn:
      return "WARN";
    case Level::Error:
      return "ERROR";
  }
  return "?";
}

}  // namespace

void SetFile(const std::filesystem::path& file) {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  std::filesystem::rename(file, file.string() + ".1", ec);
  std::lock_guard lock(g_mutex);
  g_file.open(file, std::ios::trunc);
}

void SetLevel(Level level) {
  g_level.store(level, std::memory_order_relaxed);
}

void Write(Level level, std::string_view message) {
  if (level < g_level.load(std::memory_order_relaxed)) return;
  // journald captures stderr and adds its own timestamps, so we add none.
  loghub::Append("daemon", std::string(Name(level)) + " " + std::string(message) + "\n");
  std::lock_guard lock(g_mutex);
  std::fprintf(stderr, "%-5.*s %.*s\n", static_cast<int>(Name(level).size()), Name(level).data(),
               static_cast<int>(message.size()), message.data());
  if (g_file.is_open()) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%F %T", &local);
    g_file << std::format("{} {:<5} {}", stamp, Name(level), message) << std::endl;
  }
}

}  // namespace mira::log
