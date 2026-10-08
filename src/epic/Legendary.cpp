#include "epic/Legendary.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>

#include "core/Json.h"
#include "core/Log.h"
#include "core/StoreErrors.h"
#include "core/Strings.h"
#include "runner/Curl.h"
#include "runner/Exec.h"

namespace mira::epic {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

}  // namespace

std::filesystem::path ManagedLegendaryPath(const config::Config& config) {
  return config.File().parent_path() / "tools" / "legendary";
}

const runner::StoreTool kTool = {"epic", "Epic Games", "legendary", "epic.legendary_bin", ManagedLegendaryPath};

std::filesystem::path LegendaryConfigDir() {
  if (const char* own = std::getenv("LEGENDARY_CONFIG_PATH"); own && *own) return own;
  const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME");
  fs::path config_dir;
  if (xdg_config_home && *xdg_config_home) {
    config_dir = xdg_config_home;
  } else {
    const char* home = std::getenv("HOME");
    config_dir = (home && *home ? fs::path(home) : fs::path()) / ".config";
  }
  return config_dir / "legendary";
}

std::filesystem::path LegendaryMetadataFile(const std::string& app_name) {
  return LegendaryConfigDir() / "metadata" / (app_name + ".json");
}

runner::ToolStatus DetectLegendary(const config::Config& config) { return runner::DetectTool(config, kTool); }

Result<void> InstallLegendaryBinary(const config::Config& config, const runner::ReleaseAsset& asset) {
  const fs::path target = ManagedLegendaryPath(config);
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (ec) return Err("install_dir_failed", ec.message());

  // Downloaded beside the target, so a failed or stalled download never leaves a broken binary there.
  const fs::path part = target.string() + ".part";
  if (auto downloaded = runner::CurlDownload(asset.download_url, part); !downloaded) return downloaded;

  fs::rename(part, target, ec);
  if (ec) {
    fs::remove(part, ec);
    return Err("install_failed", ec.message());
  }

  // Legendary's releases ship no checksum, so it's installed unverified.
  log::Warn("no checksum available for legendary {}, installing unverified", asset.tag);

  fs::permissions(target,
                  fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                      fs::perms::others_exec,
                  ec);
  if (ec) return Err("chmod_failed", ec.message());
  return {};
}

Result<std::string> RunLegendary(const config::Config& config, std::vector<std::string> args, const runner::OutputFn& on_output) {
  return runner::RunTool(config, kTool, args, on_output);
}

Result<json> RunLegendaryJson(const config::Config& config, std::vector<std::string> args) {
  args.push_back("--json");
  const Result<std::string> output = RunLegendary(config, args);
  if (!output) return std::unexpected(output.error());

  const json parsed = core::ParseJsonTail(*output);
  if (parsed.is_discarded()) return Err("legendary_json_error", "legendary's output wasn't valid JSON");
  return parsed;
}

runner::AuthStatus Status(const config::Config& config) {
  runner::AuthStatus status;
  status.tool = DetectLegendary(config);
  if (!status.tool.installed) return status;

  // The session legendary keeps, read directly: `legendary status` signs in
  // online and lists every owned game, about a second on each page open.
  std::ifstream file(LegendaryConfigDir() / "user.json");
  const json user = json::parse(file, nullptr, false);
  if (!user.is_object()) return status;
  const std::string account = user.value("displayName", std::string());
  // ISO 8601 in UTC, so the text order is the time order. Past it legendary has to sign in again.
  const std::string expires = user.value("refresh_expires_at", std::string());
  const std::string now = std::format("{:%FT%T}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
  if (!account.empty() && (expires.empty() || expires > now)) {
    status.authenticated = true;
    status.account = account;
  }
  return status;
}

Result<void> CheckReady(const config::Config& config) {
  return runner::CheckStoreReady(kTool, Status(config));
}

Result<void> Login(const config::Config& config, const std::string& pasted) {
  std::string code = strings::Trim(pasted);
  if (code.starts_with('{')) {
    const json page = json::parse(code, nullptr, false);
    if (page.is_discarded() || !page.contains("authorizationCode") || !page["authorizationCode"].is_string()) {
      return Err("invalid_code", "that looks like JSON but has no \"authorizationCode\" field");
    }
    code = page["authorizationCode"].get<std::string>();
  }
  if (code.empty()) return Err("invalid_code", "no code entered");
  // legendary's own exit code is not trustworthy here: `auth --code` with an
  // invalid or expired code still exits 0, only reporting the failure as an
  // "[cli] ERROR: Login attempt failed" line in its output, so success is
  // checked through status afterwards.
  if (auto output = RunLegendary(config, {"auth", "--code", code}); !output) {
    return std::unexpected(output.error());
  }
  if (const runner::AuthStatus status = Status(config); !status.authenticated) {
    return Err("login_failed", "legendary didn't accept that code. It may be wrong, expired, or already used");
  }
  return {};
}

Result<void> Logout(const config::Config& config) {
  const Result<std::string> output = RunLegendary(config, {"auth", "--delete"});
  if (!output) return std::unexpected(output.error());
  return {};
}

}  // namespace mira::epic
