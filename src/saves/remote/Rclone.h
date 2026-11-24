#pragma once

#include <filesystem>
#include <string_view>

#include "config/Config.h"
#include "core/Result.h"
#include "runner/StoreTool.h"

namespace mira::saves::remote {

// Where Mira's own rclone is, or would be installed.
std::filesystem::path ManagedRclonePath(const config::Config& config);

// rclone below 1.76 isn't known to refuse a redirect from HTTPS to HTTP.
bool NewEnough(std::string_view version);

// The setting, then Mira's copy, then $PATH; a copy older than NewEnough is not installed.
runner::ToolStatus DetectRclone(const config::Config& config);

// Downloads the latest rclone release into Mira's tools folder.
Result<void> InstallRclone(const config::Config& config);

}  // namespace mira::saves::remote
