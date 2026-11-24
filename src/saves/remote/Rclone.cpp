#include "saves/remote/Rclone.h"

#include <cctype>
#include <charconv>
#include <system_error>

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

}  // namespace mira::saves::remote
