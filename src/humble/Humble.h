#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "runner/Downloader.h"
#include "runner/StoreTool.h"

// Wraps humble-cli (github.com/smbl64/humble-cli, unofficial). Not built
// on library::ILibrarySource like epic/steam/gog/itch: Humble Bundle has
// no "installed" concept, just purchased bundles of downloadable files,
// so there's no catalog/install/update lifecycle to wrap: GET
// /v1/humble/library lists what's purchased, POST /v1/humble/download
// fetches files from one bundle into a plain directory.
namespace mira::humble {

std::filesystem::path ManagedHumbleCliPath(const config::Config& config);

runner::ToolStatus DetectHumbleCli(const config::Config& config);

Result<void> InstallHumbleCliBinary(const config::Config& config, const runner::ReleaseAsset& asset);

Result<std::string> RunHumbleCli(const config::Config& config, std::vector<std::string> args);

// Log in here in a browser, then copy its _simpleauth_sess cookie.
inline constexpr std::string_view kLoginUrl = "https://www.humblebundle.com/login";

// Unlike epic/gog, there's no separate "is a token stored" file to check:
// humble-cli owns its own config file entirely. The only way to know if
// it's authenticated is to actually ask it something (`list`), so this
// one *does* shell out, unlike epic::Status/gog::Status's "installed
// check is free" posture.
runner::AuthStatus Status(const config::Config& config);

// `session_key` is the _simpleauth_sess cookie value, copied from a
// logged-in browser session (documented in humble-cli's own README).
// There's no login URL/code flow the way Epic/GOG have).
Result<void> Login(const config::Config& config, const std::string& session_key);

struct BundleSummary {
  std::string key;
  std::string name;
  bool claimed = false;
};

Result<std::vector<BundleSummary>> ListBundles(const config::Config& config);

// Where one bundle's downloaded items land: "<humble.download_root>/<bundle_key>/".
// Not auto-imported as a model::Game -- Humble's items are arbitrary
// archives/installers, not a provisioned prefix (see Humble.h's class
// comment) -- the existing manual-add flow (POST /v1/games, or
// AutoSetup's detection of the extracted folder) takes it from there.
std::filesystem::path DownloadDir(const config::Config& config, const std::string& bundle_key);

// Runs `humble-cli download <bundle_key> --cur-dir` into DownloadDir(),
// optionally narrowed by `item_numbers` (humble-cli's own "1,3,5-7" range
// syntax, passed through as-is). Blocking, same convention as every other
// install-shaped call here -- the caller runs it on a detached thread.
// Returns whether anything actually landed on disk: a redeemed
// Steam-key-only entry has no Humble-hosted files at all, and humble-cli
// exits 0 printing "Nothing to download" for those rather than failing.
Result<bool> Download(const config::Config& config, const std::string& bundle_key, const std::string& item_numbers);

}  // namespace mira::humble
