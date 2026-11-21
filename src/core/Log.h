#pragma once

#include <filesystem>
#include <format>
#include <string_view>

namespace mira::log {

enum class Level { Debug, Info, Warn, Error };

void SetLevel(Level level);
// Also writes each line to `file`, keeping the previous run's as <file>.1.
void SetFile(const std::filesystem::path& file);
void Write(Level level, std::string_view message);

template <typename... Args>
void Debug(std::format_string<Args...> fmt, Args&&... args) {
  Write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void Info(std::format_string<Args...> fmt, Args&&... args) {
  Write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void Warn(std::format_string<Args...> fmt, Args&&... args) {
  Write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void Error(std::format_string<Args...> fmt, Args&&... args) {
  Write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace mira::log
