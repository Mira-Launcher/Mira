#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "runner/Downloader.h"
#include "runner/StoreTool.h"

namespace mira::itch {

std::filesystem::path ManagedButlerPath(const config::Config& config);

runner::ToolStatus DetectButler(const config::Config& config);

Result<void> InstallButlerBinary(const config::Config& config, const runner::ReleaseAsset& asset);

// Mira-managed storage for the itch.io API key butlerd's Profile.
// LoginWithAPIKey needs -- butlerd itself is handed this key fresh at
// connect time (Butlerd.cpp), it isn't asked to remember it the way
// Legendary/gogdl persist their own tokens on disk, so Mira keeps it
// instead.
std::filesystem::path ApiKeyFile(const config::Config& config);

// Where an itch.io API key is made. Keys don't expire.
inline constexpr std::string_view kApiKeysUrl = "https://itch.io/user/settings/api-keys";

// installed-only if butler itself isn't present; otherwise just whether a
// key is stored -- never connects to butlerd just to check this, matching
// gog::Status's "no cheap status call" posture (here doubly true: starting
// butlerd is a real subprocess spawn, not a quick one-shot command).
runner::AuthStatus Status(const config::Config& config);

// Ok when the tool is installed and the account signed in, else the error that says what to set up.
Result<void> CheckReady(const config::Config& config);

// Stores `api_key` and verifies it immediately via Profile.LoginWithAPIKey
// (so a bad key is caught right here, not on the first real catalog
// call).
Result<void> Login(const config::Config& config, const std::string& api_key);

// The text itself when it is shaped like an API key (one run of letters and digits), else nullopt.
std::optional<std::string> FindKey(std::string_view text);

Result<void> Logout(const config::Config& config);

// Every Fetch.ProfileOwnedKeys/Install.Queue call needs a numeric
// profileId (confirmed against butlerd's own spec) -- there's no
// separate persisted profile id, so this re-runs Profile.LoginWithAPIKey
// with the stored key each time and reads .profile.id back.
Result<std::int64_t> CurrentProfileId(const config::Config& config);

// Install.Queue needs a registered "install location" id when installing
// a title for the first time ("installLocationId must be set"). Idempotent,
// so it's safe to call before every install.
Result<void> EnsureInstallLocation(const config::Config& config);

// A collection link's id: "https://itch.io/c/<id>/<slug>", the same
// without the slug or scheme, or a bare id.
std::optional<std::int64_t> ParseCollectionLink(std::string_view link);

struct ItchCollection {
  std::int64_t id = 0;
  std::string title;
  std::int64_t games_count = 0;
  bool own = false;  // the account's own, not added by link
};

// The account's own collections, then the ones added by link
// (itch.collections). An added one that can't be read is left out.
Result<std::vector<ItchCollection>> ListCollections(const config::Config& config);

// Checks the collection can be read, then adds it to itch.collections.
Result<ItchCollection> AddCollection(config::Config& config, std::string_view link);

Result<void> RemoveCollection(config::Config& config, std::int64_t id);

}  // namespace mira::itch
