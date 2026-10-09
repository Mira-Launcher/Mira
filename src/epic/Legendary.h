#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <json.hpp>

#include "config/Config.h"
#include "core/Result.h"
#include "runner/Downloader.h"
#include "runner/StoreTool.h"
#include "runner/Exec.h"

// Epic Games Store support: wraps Legendary, a native-Linux Epic CLI client,
// for everything protocol-shaped (auth, catalog, install, update, uninstall).
// Mira never runs Legendary's own `legendary launch`: once a game is
// installed, it's launched through Mira's own Wine/Proton runners like any
// other Windows game (see docs/architecture.md). This file only knows how to
// find or fetch the `legendary` binary and run it; it has no game-library
// knowledge (see EpicImporter/EpicInstaller for that).
namespace mira::epic {

// Where Mira's own managed download lives, if it ever fetches one. It never
// requires the file to actually exist.
std::filesystem::path ManagedLegendaryPath(const config::Config& config);

// Legendary's own per-title metadata cache file (populated by `legendary
// list`), read directly by metadata::FetchEpicOwned rather than shelling out
// again, just reading another program's own cache, same posture as reading
// Lutris's pga.db. $XDG_CONFIG_HOME/legendary (default ~/.config/legendary)
// is Legendary's own config dir, independent of Mira's and not affected by
// epic.legendary_bin, which only overrides where the binary lives.
std::filesystem::path LegendaryMetadataFile(const std::string& app_name);

// Legendary's own config folder: $LEGENDARY_CONFIG_PATH, else the one described above.
std::filesystem::path LegendaryConfigDir();

// Detects legendary in this order: epic.legendary_bin override, Mira's own
// managed download, then $PATH. Never itself requires legendary to already
// work, so it is safe to call before anything is set up, unlike everything below.
runner::ToolStatus DetectLegendary(const config::Config& config);

// Downloads legendary's latest matching GitHub release asset (a standalone
// binary, not an archive) into ManagedLegendaryPath via runner::InstallToolBinary.
// Reuses runner::ListReleases (config kind "legendary").
Result<void> InstallLegendaryBinary(const config::Config& config, const runner::ReleaseAsset& asset);

// Runs `legendary <args...>` using whatever DetectLegendary resolved.
// Err("legendary_missing", ...) up front if nothing resolved, pointing at
// "mira epic setup", and never assumes legendary is there. Returns combined
// stdout+stderr; Err("legendary_failed", ...) on a nonzero exit.
Result<std::string> RunLegendary(const config::Config& config, std::vector<std::string> args,
                               const runner::OutputFn& on_output = {});

// Like RunLegendary, but appends --json and parses stdout as JSON.
Result<nlohmann::json> RunLegendaryJson(const config::Config& config, std::vector<std::string> args);

// Legendary's own documented manual-login URL (its README's instructions for
// an environment with no browser legendary itself can open, which is the case here,
// since RunLegendary always executes on mirad's side, headless). A fixed,
// public URL, not a secret; only changes if Epic ever rotates the OAuth
// client id legendary itself uses.
inline constexpr std::string_view kLoginUrl =
    "https://www.epicgames.com/id/login?redirectUrl=https%3A%2F%2Fwww.epicgames.com%2Fid%2Fapi%2Fredirect%3FclientId"
    "%3D34a02cf8f4414e29b15921876da36f9a%26responseType%3Dcode";

// Whether legendary is installed and signed in, from its saved session
// (user.json) without running it. Never itself errors: "not installed" and
// "not authenticated" are both just fields on the result, not failures.
runner::AuthStatus Status(const config::Config& config);

// Ok when the tool is installed and the account signed in, else the error that says what to set up.
Result<void> CheckReady(const config::Config& config);

// Runs `legendary auth --code <code>`, the headless login path (see
// kLoginUrl's comment): the user visits kLoginUrl in their own browser,
// pastes back the code it shows. Err("legendary_missing", ...) if legendary
// isn't installed, same as everything else here. `pasted` may also be the
// whole JSON page kLoginUrl ends on; the code is pulled out of it.
Result<void> Login(const config::Config& config, const std::string& pasted);

// The authorization code in copied text: the JSON page, Firefox's JSON viewer
// copied as text, or the bare code. nullopt when there's none.
std::optional<std::string> FindCode(std::string_view text);

Result<void> Logout(const config::Config& config);

}  // namespace mira::epic
