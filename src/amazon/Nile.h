#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <json.hpp>

#include "config/Config.h"
#include "core/Result.h"
#include "runner/Downloader.h"
#include "runner/StoreTool.h"
#include "runner/Exec.h"

// Amazon Games (Prime Gaming) support: wraps nile, Heroic's Amazon CLI
// client, for login, library and downloads. Installed games run through
// Mira's own runners, never `nile launch`.
namespace mira::amazon {

runner::ToolStatus DetectNile(const config::Config& config);

Result<void> InstallNileBinary(const config::Config& config, const runner::ReleaseAsset& asset);

// nile's own config directory ($NILE_CONFIG_PATH, $XDG_CONFIG_HOME or
// ~/.config, plus "nile"): library.json and installed.json live here.
std::filesystem::path NileConfigDir();

// A JSON file from NileConfigDir(), or null if missing/unreadable.
nlohmann::json ReadNileFile(const std::string& name);

Result<std::string> RunNile(const config::Config& config, std::vector<std::string> args,
                               const runner::OutputFn& on_output = {});

runner::AuthStatus Status(const config::Config& config);

// Ok when the tool is installed and the account signed in, else the error that says what to set up.
Result<void> CheckReady(const config::Config& config);

// Starts a login: returns the Amazon URL to open. The PKCE values nile
// generated are kept in memory until FinishLogin.
Result<std::string> BeginLogin(const config::Config& config);

// `redirect` is the amazon.com URL the login ended on, or just its
// openid.oa2.authorization_code value.
Result<void> FinishLogin(const config::Config& config, const std::string& redirect);

Result<void> Logout(const config::Config& config);

}  // namespace mira::amazon
