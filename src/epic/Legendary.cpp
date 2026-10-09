#include "epic/Legendary.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <ranges>
#include <regex>
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
  return config.File().parent_path() / "tools" / "legendary" / "legendary";
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

namespace {
// The pre-folder layout kept the binary as a plain file at tools/legendary: moves it into the folder.
Result<void> MoveOldLayout(const config::Config& config) {
  const fs::path old_file = config.File().parent_path() / "tools" / "legendary";
  std::error_code ec;
  if (!fs::is_regular_file(old_file, ec)) return {};
  const fs::path moving = old_file.string() + ".moving";
  fs::rename(old_file, moving, ec);
  if (!ec) fs::create_directories(old_file, ec);
  if (!ec) fs::rename(moving, ManagedLegendaryPath(config), ec);
  if (ec) return Err("install_dir_failed", std::format("couldn't move the old Legendary into {}: {}", old_file.string(), ec.message()));
  return {};
}
}  // namespace

runner::ToolStatus DetectLegendary(const config::Config& config) {
  [[maybe_unused]] auto moved = MoveOldLayout(config);
  return runner::DetectTool(config, kTool);
}

Result<void> InstallLegendaryBinary(const config::Config& config, const runner::ReleaseAsset& asset) {
  if (auto moved = MoveOldLayout(config); !moved) return moved;
  auto installed = runner::InstallToolBinary(config, "legendary", asset, "legendary");
  if (!installed) return std::unexpected(installed.error());
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

std::optional<std::string> FindCode(std::string_view text) {
  static const std::regex field(R"re(authorizationCode"?\s*:?\s*"([0-9a-fA-F]{32})")re");
  static const std::regex bare(R"(^\s*([0-9a-fA-F]{32})\s*$)");
  std::match_results<std::string_view::const_iterator> match;
  if (std::regex_search(text.begin(), text.end(), match, field) ||
      std::regex_match(text.begin(), text.end(), match, bare)) {
    return match[1].str();
  }
  return std::nullopt;
}

Result<void> Login(const config::Config& config, const std::string& pasted) {
  std::string code = FindCode(pasted).value_or(strings::Trim(pasted));
  if (code.starts_with('{')) return Err("invalid_code", "that looks like JSON but has no \"authorizationCode\" field");
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
