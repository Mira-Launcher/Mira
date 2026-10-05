#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "runner/Downloader.h"
#include "runner/StoreTool.h"
#include "runner/Exec.h"

// Wraps gogdl (github.com/Heroic-Games-Launcher/heroic-gogdl), the GOG
// downloader Heroic itself uses. It's a self-executing Python zipapp, not
// a static binary like Legendary/butler -- needs a system python3.
//
// gogdl has no "list what's installed"/"list owned" subcommand at all
// (confirmed via `gogdl --help`); "import" only identifies a game already
// unpacked at a given path. Catalog/ownership instead comes from GOG's
// own embed.gog.com API directly (see GogSource.cpp), using the token
// gogdl's own `auth` step already obtained.
namespace mira::gog {

std::filesystem::path ManagedGogPath(const config::Config& config);

runner::ToolStatus DetectGog(const config::Config& config);

Result<void> InstallGogBinary(const config::Config& config, const runner::ReleaseAsset& asset);

// Where Mira tells gogdl to keep its own OAuth token file
// (--auth-config-path, passed to every invocation below). Mira-managed,
// alongside settings.toml -- not shared with any other GOG client.
std::filesystem::path AuthConfigPath(const config::Config& config);

// Runs gogdl with --auth-config-path already inserted. Blocking, same
// convention as epic::RunLegendary.
Result<std::string> RunGogdl(const config::Config& config, std::vector<std::string> args,
                               const runner::OutputFn& on_output = {});

// installed-only if gogdl itself isn't present; otherwise checks whether
// AuthConfigPath holds an unexpired access token -- never shells out just
// to check this, unlike epic::Status (gogdl has no cheap "status" call).
runner::AuthStatus Status(const config::Config& config);

// Ok when the tool is installed and the account signed in, else the error that says what to set up.
Result<void> CheckReady(const config::Config& config);

// GOG Galaxy's own public login page, the one gogdl's client_id belongs to.
// It ends on a blank page whose URL carries the code.
inline constexpr std::string_view kLoginUrl =
    "https://auth.gog.com/auth?client_id=46899977096215655&redirect_uri=https%3A%2F%2Fembed.gog.com"
    "%2Fon_login_success%3Forigin%3Dclient&response_type=code&layout=client2";

// `pasted` is the authorization code from kLoginUrl's redirect, or that
// whole redirect URL. gogdl bakes in GOG Galaxy's own public
// client_id/client_secret (confirmed in heroic-gogdl's auth.py) -- Mira
// never needs its own.
Result<void> Login(const config::Config& config, const std::string& pasted);

// Just removes AuthConfigPath -- gogdl has no "auth --delete" the way
// Legendary does.
Result<void> Logout(const config::Config& config);

// The access token from AuthConfigPath, refreshing once via a bare
// `gogdl auth` call (no --code) if it looks expired -- gogdl's own load
// path already knows how to use its stored refresh_token, confirmed in
// auth.py's is_credential_expired/refresh_credentials; Mira just triggers
// that load by invoking gogdl. Returns Err("not_authenticated", ...) if
// refreshing doesn't produce a usable token either.
Result<std::string> AccessToken(const config::Config& config);

}  // namespace mira::gog
