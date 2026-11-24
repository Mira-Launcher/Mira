#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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

// A place rclone moves saves to: a section of Mira's own rclone config file.
struct Remote {
  std::filesystem::path config_file;
  std::string root;               // "primary:saves"
  std::filesystem::path ca_file;  // a certificate to trust besides the system's; may be empty
};

// What a failed run means, from its exit code and log: remote_offline, remote_sign_in, remote_full,
// remote_certificate, remote_host_key, remote_insecure, remote_not_found or remote_failed.
Error RcloneError(int exit_code, std::string_view log);

// Runs `rclone <args>` on `remote`, with Mira's config file only and no inherited RCLONE_*
// variables, and returns its stdout.
Result<std::string> RunRclone(const config::Config& config, const Remote& remote,
                              std::vector<std::string> args);

}  // namespace mira::saves::remote
