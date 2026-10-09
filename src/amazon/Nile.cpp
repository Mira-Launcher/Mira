#include "amazon/Nile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <ranges>
#include <regex>

#include "core/Json.h"
#include "core/StoreErrors.h"
#include "runner/Exec.h"

namespace mira::amazon {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

fs::path ManagedNilePath(const config::Config& config) {
  return config.File().parent_path() / "tools" / "amazon" / "nile";
}

const runner::StoreTool kTool = {"amazon", "Amazon Games", "nile", "amazon.nile_bin", ManagedNilePath};

std::mutex pending_mutex;
std::optional<json> pending_login;  // client_id, code_verifier, serial

}  // namespace

runner::ToolStatus DetectNile(const config::Config& config) { return runner::DetectTool(config, kTool); }

Result<void> InstallNileBinary(const config::Config& config, const runner::ReleaseAsset& asset) {
  auto installed = runner::InstallToolBinary(config, "amazon", asset, "nile");
  if (!installed) return std::unexpected(installed.error());
  return {};
}

fs::path NileConfigDir() {
  for (const char* var : {"NILE_CONFIG_PATH", "XDG_CONFIG_HOME"}) {
    if (const char* value = std::getenv(var); value && *value) return fs::path(value) / "nile";
  }
  const char* home = std::getenv("HOME");
  return fs::path(home ? home : "") / ".config" / "nile";
}

json ReadNileFile(const std::string& name) {
  std::ifstream file(NileConfigDir() / name);
  if (!file) return nullptr;
  const json parsed = json::parse(file, nullptr, false);
  return parsed.is_discarded() ? json(nullptr) : parsed;
}

Result<std::string> RunNile(const config::Config& config, std::vector<std::string> args, const runner::OutputFn& on_output) {
  return runner::RunTool(config, kTool, args, on_output);
}

runner::AuthStatus Status(const config::Config& config) {
  runner::AuthStatus status;
  status.tool = DetectNile(config);
  if (!status.tool.installed) return status;
  const Result<std::string> output = RunNile(config, {"auth", "--status"});
  if (!output) return status;
  const json parsed = core::ParseJsonTail(*output);
  status.authenticated = parsed.is_object() && parsed.value("LoggedIn", false);
  return status;
}

Result<void> CheckReady(const config::Config& config) {
  return runner::CheckStoreReady(kTool, Status(config));
}

Result<std::string> BeginLogin(const config::Config& config) {
  const Result<std::string> output = RunNile(config, {"auth", "--login", "--non-interactive"});
  if (!output) return std::unexpected(output.error());
  const json parsed = core::ParseJsonTail(*output);
  if (!parsed.is_object() || !parsed.contains("url")) {
    return Status(config).authenticated ? Err("already_authenticated", "already logged in to Amazon")
                                        : Err("nile_json_error", "nile didn't return a login URL");
  }
  const std::lock_guard lock(pending_mutex);
  pending_login = parsed;
  return parsed.value("url", std::string());
}

std::optional<std::string> FindCode(std::string_view text) {
  static const std::regex code(R"(openid\.oa2\.authorization_code=([^&\s]+))");
  std::match_results<std::string_view::const_iterator> match;
  if (!std::regex_search(text.begin(), text.end(), match, code)) return std::nullopt;
  return match[1].str();
}

Result<void> FinishLogin(const config::Config& config, const std::string& redirect) {
  json pending;
  {
    const std::lock_guard lock(pending_mutex);
    if (!pending_login) {
      return Err("no_login_pending", "no Amazon Games sign-in is in progress", "Start signing in again.",
                 Fix::Source("amazon", "login"));
    }
    pending = *pending_login;
  }
  const std::string code = FindCode(redirect).value_or(redirect);
  if (auto registered = RunNile(config, {"register", "--code", code, "--client-id", pending.value("client_id", ""),
                                         "--code-verifier", pending.value("code_verifier", ""), "--serial",
                                         pending.value("serial", "")});
      !registered) {
    return Err("login_failed", "nile couldn't register this device. The code may be wrong or expired");
  }
  {
    const std::lock_guard lock(pending_mutex);
    pending_login.reset();
  }
  if (!Status(config).authenticated) {
    return Err("login_failed", "nile didn't accept that code. It may be wrong, expired, or already used");
  }
  return {};
}

Result<void> Logout(const config::Config& config) {
  if (auto output = RunNile(config, {"auth", "--logout"}); !output) return std::unexpected(output.error());
  return {};
}

}  // namespace mira::amazon
