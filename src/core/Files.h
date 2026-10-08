#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace mira::files {

// The whole file as bytes, or nullopt when it can't be opened.
std::optional<std::string> ReadFile(const std::filesystem::path& path);

}  // namespace mira::files
