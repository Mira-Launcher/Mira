#include "gog/Gog.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>

#include <json.hpp>

#include "core/Log.h"
#include "core/StoreErrors.h"
#include "runner/Exec.h"

namespace mira::gog {
namespace {
namespace fs = std::filesystem;
using nlohmann::json;

std::string Trim(std::string text) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  text.erase(text.begin(), std::ranges::find_if(text, not_space));
  text.erase(std::ranges::find_if(text | std::views::reverse, not_space).base(), text.end());
  return text;
}

std::optional<json> ReadAuthConfig(const config::Config& config) {
  std::ifstream file(AuthConfigPath(config));
  if (!file) return std::nullopt;
  const json parsed = json::parse(file, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object() || parsed.empty()) return std::nullopt;

  // gogdl nests the token fields one level down, keyed by client_id:
  // {"<client_id>": {"access_token": ..., ...}}. This isn't documented.
  const json& inner = parsed.begin().value();
  if (!inner.is_object()) return std::nullopt;
  return inner;
}

}  // namespace

std::filesystem::path ManagedGogPath(const config::Config& config) {
  return config.File().parent_path() / "tools" / "gog" / "gogdl";
}

const runner::StoreTool kTool = {"gog", "GOG", "gogdl", "gog.gogdl_bin", ManagedGogPath};

runner::ToolStatus DetectGog(const config::Config& config) { return runner::DetectTool(config, kTool); }

Result<void> InstallGogBinary(const config::Config& config, const runner::ReleaseAsset& asset) {
  // gogdl is a Python zipapp ("#!/usr/bin/env python3" shebang), not a
  // self-contained binary, so a system
  // python3 has to actually be there for it to run at all, unlike
  // Legendary. Checked here rather than only at first use, so
  // "mira gog setup" fails with a clear, actionable error immediately
  // instead of leaving a binary that can't execute.
  if (!runner::FindOnPath("python3")) {
    return Err("python3_missing",
              "gogdl needs a system python3 to run (it's a Python zipapp, not a standalone binary). "
              "Install python3 first");
  }
  auto installed = runner::InstallToolBinary(config, "gog", asset, "gogdl");
  if (!installed) return std::unexpected(installed.error());
  return {};
}

std::filesystem::path AuthConfigPath(const config::Config& config) {
  return config.File().parent_path() / "gog-auth.json";
}

Result<std::string> RunGogdl(const config::Config& config, std::vector<std::string> args, const runner::OutputFn& on_output) {
  return runner::RunTool(config, kTool, args, on_output, {"--auth-config-path", AuthConfigPath(config).string()});
}

runner::AuthStatus Status(const config::Config& config) {
  runner::AuthStatus status;
  status.tool = DetectGog(config);
  if (!status.tool.installed) return status;

  const auto stored = ReadAuthConfig(config);
  if (!stored) return status;
  status.authenticated = stored->contains("access_token") && (*stored)["access_token"].is_string() &&
                        !(*stored)["access_token"].get<std::string>().empty();
  return status;
}

Result<void> CheckReady(const config::Config& config) {
  return runner::CheckStoreReady(kTool, Status(config));
}

Result<void> Login(const config::Config& config, const std::string& pasted) {
  std::string code = Trim(pasted);
  if (const size_t marker = code.find("code="); marker != std::string::npos) {
    code = code.substr(marker + 5);
    code = code.substr(0, code.find('&'));
  }
  if (code.empty()) return Err("invalid_code", "no code entered");
  // Same posture as epic::Login: verify by re-reading what gogdl actually
  // wrote, not by trusting a nonzero/zero exit code: gogdl's own `auth`
  // handler prints {"error": true} on a rejected code but still exits 0.
  if (auto output = RunGogdl(config, {"auth", "--code", code}); !output) {
    return std::unexpected(output.error());
  }
  if (const runner::AuthStatus status = Status(config); !status.authenticated) {
    return Err("login_failed", "gogdl didn't accept that code. It may be wrong, expired, or already used");
  }
  return {};
}

Result<void> Logout(const config::Config& config) {
  std::error_code ec;
  fs::remove(AuthConfigPath(config), ec);
  return {};
}

Result<std::string> AccessToken(const config::Config& config) {
  auto stored = ReadAuthConfig(config);
  bool expired = true;
  if (stored && stored->contains("loginTime") && (*stored)["loginTime"].is_number() && stored->contains("expires_in") &&
      (*stored)["expires_in"].is_number()) {
    const double login_time = (*stored)["loginTime"].get<double>();
    const double expires_in = (*stored)["expires_in"].get<double>();
    const double now = static_cast<double>(std::chrono::duration_cast<std::chrono::seconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count());
    expired = now >= login_time + expires_in;
  }

  if (!stored || expired) {
    // gogdl's own load path refreshes an expired token using its stored
    // refresh_token as a side effect of loading --auth-config-path at all
    // (confirmed in heroic-gogdl's auth.py: is_credential_expired/
    // refresh_credentials run before any subcommand does its own work),
    // so triggering that is as simple as invoking gogdl with no real work to
    // do.
    if (auto refreshed = RunGogdl(config, {"auth"}); !refreshed) return std::unexpected(refreshed.error());
    stored = ReadAuthConfig(config);
  }

  if (!stored || !stored->contains("access_token") || !(*stored)["access_token"].is_string()) {
    return StoreNotSignedIn("gog", "GOG");
  }
  return (*stored)["access_token"].get<std::string>();
}

}  // namespace mira::gog
