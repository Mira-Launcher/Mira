#include "saves/remote/Rclone.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <format>
#include <system_error>

#include "core/Files.h"
#include "core/Log.h"
#include "core/Strings.h"
#include "runner/Downloader.h"
#include "runner/Exec.h"

namespace mira::saves::remote {
namespace fs = std::filesystem;

fs::path ManagedRclonePath(const config::Config& config) {
  if (const auto installed = runner::InstalledToolOf(config, "rclone")) return installed->binary;
  return config.File().parent_path() / "tools" / "rclone" / "rclone";
}

bool NewEnough(std::string_view version) {
  // The first "v<major>.<minor>" in the text.
  for (size_t i = 0; i + 1 < version.size(); ++i) {
    if (version[i] != 'v' || !std::isdigit(static_cast<unsigned char>(version[i + 1]))) continue;
    const char* end = version.data() + version.size();
    int major = 0;
    int minor = 0;
    const auto [after_major, major_ec] = std::from_chars(version.data() + i + 1, end, major);
    if (major_ec != std::errc{} || after_major == end || *after_major != '.') return false;
    const auto [after_minor, minor_ec] = std::from_chars(after_major + 1, end, minor);
    if (minor_ec != std::errc{}) return false;
    return major > 1 || (major == 1 && minor >= 76);
  }
  return false;
}

runner::ToolStatus DetectRclone(const config::Config& config) {
  const auto found = [](const char* source, const std::string& path) {
    return runner::ToolStatus{
        .installed = true, .source = source, .path = path, .version = runner::ToolVersion(path)};
  };
  const std::string override_path = config.GetString("saves.rclone_bin");
  if (!override_path.empty() && fs::exists(override_path)) {
    runner::ToolStatus status = found("override", override_path);
    if (!NewEnough(status.version)) return {};
    return status;
  }
  const fs::path managed = ManagedRclonePath(config);
  if (fs::exists(managed)) return found("managed", managed.string());
  if (const auto on_path = runner::FindOnPath("rclone")) {
    runner::ToolStatus status = found("path", *on_path);
    if (NewEnough(status.version)) return status;
  }
  return {};
}

Result<void> InstallRclone(const config::Config& config) {
  const auto releases = runner::ListReleases(config, "rclone");
  if (!releases) return std::unexpected(releases.error());
  if (releases->empty()) return Err("no_release_found", "no rclone release found");
  const auto installed = runner::InstallToolBinary(config, "rclone", releases->front(), "rclone",
                                                   /*require_digest=*/true);
  if (!installed) return std::unexpected(installed.error());
  return {};
}

Error RcloneError(int exit_code, std::string_view log) {
  const auto says = [&](std::initializer_list<std::string_view> words) {
    return std::ranges::any_of(words, [&](std::string_view word) { return log.contains(word); });
  };
  if (says({"refusing to follow HTTPS to HTTP"}))
    return {.code = "remote_insecure",
            .message = "The server redirects to an unencrypted address, so nothing was sent.",
            .hint = {}};
  if (says({"x509:", "failed to verify certificate"}))
    return {.code = "remote_certificate",
            .message = "The server's certificate isn't trusted.",
            .hint = "Choose the server's certificate file if it signs its own."};
  if (says({"knownhosts:", "known_hosts_file"}))
    return {.code = "remote_host_key",
            .message = "The server's key isn't the one it was connected with.",
            .hint = "Connect again only if you know why it changed."};
  if (says({"507 Insufficient Storage", "no space left", "quotaLimitReached", "insufficient_space",
            "quota exceeded"}))
    return {.code = "remote_full", .message = "The remote has no room left.", .hint = {}};
  if (says({"401 Unauthorized", "403 Forbidden", "unable to authenticate", "invalid_grant",
            "expired_access_token", "InvalidAuthenticationToken", "SignatureDoesNotMatch",
            "InvalidAccessKeyId"}))
    return {
        .code = "remote_sign_in", .message = "The remote didn't accept the sign-in.", .hint = {}};
  if (exit_code == 3 || exit_code == 4 ||
      says({"directory not found", "object not found", "404 Not Found"}))
    return {.code = "remote_not_found", .message = "The remote doesn't have that.", .hint = {}};
  if (exit_code == 5 || says({"dial tcp", "no such host", "i/o timeout", "connection refused",
                              "connection reset", "network is unreachable"}))
    return {.code = "remote_offline", .message = "The remote can't be reached.", .hint = {}};
  return {.code = "remote_failed", .message = "rclone couldn't move the saves.", .hint = {}};
}

Result<std::string> RunRclone(const config::Config& config, const Remote& remote,
                              std::vector<std::string> args) {
  const runner::ToolStatus tool = DetectRclone(config);
  if (!tool.installed) return Err("rclone_missing", "rclone isn't installed.");

  // Logs go to a file: the output is the answer.
  std::string log_file = (fs::temp_directory_path() / "mira-rclone-XXXXXX").string();
  const int log_fd = ::mkostemp(log_file.data(), O_CLOEXEC);
  if (log_fd < 0) return Err("remote_failed", "couldn't make a temporary file");
  ::close(log_fd);
  const struct Cleanup {
    const std::string& file;
    ~Cleanup() { ::unlink(file.c_str()); }
  } cleanup{log_file};

  Command command;
  command.argv = {tool.path};
  command.argv.insert(command.argv.end(), args.begin(), args.end());
  for (const std::string& flag :
       {std::string("--config"), remote.config_file.string(), std::string("--ask-password=false"),
        std::string("--log-file"), log_file, std::string("--log-level"), std::string("NOTICE"),
        std::string("--retries"), std::string("2"), std::string("--low-level-retries"),
        std::string("3"), std::string("--contimeout"), std::string("10s"), std::string("--timeout"),
        std::string("60s")})
    command.argv.push_back(flag);
  if (!remote.ca_file.empty()) {
    command.argv.emplace_back("--ca-cert");
    command.argv.push_back(remote.ca_file.string());
  }
  command.unset_prefixes = {"RCLONE_"};
  const auto run = runner::RunAndWait(command);
  if (!run) return std::unexpected(run.error());
  if (run->exit_code == 0) return run->output;
  // Never logged: its lines can hold addresses with signatures in them.
  const std::string text = files::ReadFile(log_file).value_or("") + run->output;
  const Error error = RcloneError(run->exit_code, text);
  log::Warn("rclone {} exited {}: {}", args.front(), run->exit_code, error.code);
  return std::unexpected(error);
}

}  // namespace mira::saves::remote
