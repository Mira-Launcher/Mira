#include "steam/SteamDetector.h"

#include <algorithm>
#include <charconv>
#include <climits>
#include <fstream>
#include <sstream>

#include "core/Files.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "steam/Vdf.h"

namespace mira::steam {
namespace {
namespace fs = std::filesystem;

// Fixed across every Steam installation: Valve's own redistributable
// bundler, not a game.
constexpr std::string_view kRedistAppId = "228980";

// A VDF leaf as a number; 0 when missing or not one.
std::int64_t ParseInt64(const VdfValue* value) {
  std::int64_t number = 0;
  if (value == nullptr || !value->scalar) return 0;
  const std::string& text = *value->scalar;
  if (std::from_chars(text.data(), text.data() + text.size(), number).ec != std::errc()) return 0;
  return number;
}

// The SteamID64 of the account flagged MostRecent in loginusers.vdf, else
// of the newest sign-in; empty when there is none.
std::string LastSignedIn(const fs::path& steam_root) {
  const auto text = files::ReadFile(steam_root / "config" / "loginusers.vdf");
  if (!text) return {};
  const auto parsed = ParseVdf(*text);
  const VdfValue* users = parsed ? parsed->Get({"users"}) : nullptr;
  if (users == nullptr) return {};
  std::string account;
  std::int64_t newest = -1;
  for (const auto& [id, user] : users->children) {
    const VdfValue* recent = user.Get({"MostRecent"});
    const std::int64_t stamp =
        recent != nullptr && recent->scalar == "1" ? INT64_MAX : ParseInt64(user.Get({"Timestamp"}));
    if (stamp > newest) {
      newest = stamp;
      account = id;
    }
  }
  return account;
}

// True if `install_dir` looks like a compatibility tool (Proton, Steam Linux
// Runtime) rather than a game: it ships its own "proton" launcher script or
// a runtime entry point at its top level. Structural, not name-matched, so
// it doesn't need updating as new Proton/runtime versions ship.
bool LooksLikeSteamTooling(const fs::path& install_dir) {
  std::error_code ec;
  if (fs::exists(install_dir / "proton", ec)) return true;
  if (fs::exists(install_dir / "_v2-entry-point", ec)) return true;
  return false;
}

}  // namespace

std::optional<fs::path> FindSteamRoot(const config::Config& config) {
  std::vector<fs::path> candidates;
  if (const fs::path configured = config.GetPath("steam.root"); !configured.empty()) {
    candidates.push_back(configured);
  }
  candidates.push_back(paths::Expand("~/.steam/steam"));
  candidates.push_back(paths::Expand("~/.local/share/Steam"));

  std::error_code ec;
  for (const fs::path& candidate : candidates) {
    if (fs::is_directory(candidate / "steamapps", ec)) return candidate;
  }
  return std::nullopt;
}

std::vector<fs::path> LibraryFolders(const fs::path& steam_root) {
  // steam_root is very commonly a symlink to one of the paths
  // libraryfolders.vdf itself lists (~/.steam/steam -> ~/.local/share/Steam
  // is the standard layout), de-duplicated by what they resolve to on
  // disk, not by string equality, or every app in the default library gets
  // listed twice.
  std::error_code ec;
  std::vector<fs::path> folders = {steam_root};
  std::vector<fs::path> resolved = {fs::weakly_canonical(steam_root, ec)};

  const auto text = files::ReadFile(steam_root / "steamapps" / "libraryfolders.vdf");
  if (!text) return folders;
  const auto parsed = ParseVdf(*text);
  if (!parsed) return folders;

  const VdfValue* root = parsed->Get({"libraryfolders"});
  if (root == nullptr || !root->IsObject()) return folders;

  for (const auto& [key, entry] : root->children) {
    const VdfValue* path = entry.Get({"path"});
    if (path == nullptr || !path->scalar) continue;
    const fs::path candidate(*path->scalar);
    const fs::path canonical = fs::weakly_canonical(candidate, ec);
    if (!ec && std::ranges::find(resolved, canonical) != resolved.end()) continue;
    folders.push_back(candidate);
    resolved.push_back(canonical);
  }
  return folders;
}

std::optional<ProtonCompatInfo> ResolveProtonCompatInfo(const fs::path& compat_data_dir) {
  const auto text = files::ReadFile(compat_data_dir / "config_info");
  if (!text) return std::nullopt;

  // config_info is line-oriented, not VDF. Line 2 (0-indexed 1) is the
  // compat tool's own "files/share/fonts/" directory: three parent_path()
  // calls off of it (fonts -> share -> files -> the tool's own root, where
  // its "proton" script lives) is the same resolution Proton-adjacent tools
  // (protontricks and others) use, since Steam doesn't expose this any other
  // way short of parsing its own C++ source. Line 4 (0-indexed 3) is the
  // Steam client install path Steam itself passed as
  // STEAM_COMPAT_CLIENT_INSTALL_PATH, and reusing it verbatim means Proton
  // sees exactly the same environment Steam gave it.
  const std::vector<std::string> lines = strings::Split(*text, '\n');
  if (lines.size() < 4) return std::nullopt;

  // A trailing '/' makes std::filesystem::path treat the last component as
  // an empty pseudo-element, so parent_path() needs one extra call to get
  // past it, so it is stripped explicitly instead, so "fonts -> share -> files ->
  // tool root" is exactly three parent_path() calls, not a fragile four.
  std::string trimmed = strings::Trim(lines[1]);
  while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
  fs::path fonts_dir(trimmed);
  if (fonts_dir.empty()) return std::nullopt;
  const fs::path tool_root = fonts_dir.parent_path().parent_path().parent_path();
  const fs::path proton = tool_root / "proton";

  std::error_code ec;
  if (!fs::exists(proton, ec)) return std::nullopt;

  const fs::path client_install_path(strings::Trim(lines[3]));
  if (client_install_path.empty()) return std::nullopt;

  return ProtonCompatInfo{.proton_path = proton, .client_install_path = client_install_path};
}

std::vector<SteamApp> ListApps(const fs::path& steam_root) {
  std::vector<SteamApp> apps;

  for (const fs::path& library : LibraryFolders(steam_root)) {
    const fs::path steamapps = library / "steamapps";
    std::error_code ec;
    if (!fs::is_directory(steamapps, ec)) continue;

    for (const auto& entry : fs::directory_iterator(steamapps, fs::directory_options::skip_permission_denied, ec)) {
      const std::string filename = entry.path().filename().string();
      if (!filename.starts_with("appmanifest_") || !filename.ends_with(".acf")) continue;

      const auto text = files::ReadFile(entry.path());
      if (!text) continue;
      const auto parsed = ParseVdf(*text);
      if (!parsed) continue;

      const VdfValue* state = parsed->Get({"appstate"});
      if (state == nullptr) continue;
      const VdfValue* appid = state->Get({"appid"});
      const VdfValue* name = state->Get({"name"});
      const VdfValue* installdir = state->Get({"installdir"});
      if (appid == nullptr || !appid->scalar || installdir == nullptr || !installdir->scalar) continue;
      if (*appid->scalar == kRedistAppId) continue;
      if (name && name->scalar && name->scalar->starts_with("Steam Linux Runtime")) continue;

      SteamApp app;
      app.appid = *appid->scalar;
      app.name = name && name->scalar ? *name->scalar : app.appid;
      app.install_dir = steamapps / "common" / *installdir->scalar;
      app.steam_root = steam_root;
      app.library_root = library;

      if (!fs::is_directory(app.install_dir, ec)) continue;  // manifest with no actual content on disk
      if (LooksLikeSteamTooling(app.install_dir)) continue;

      app.compat_data_dir = steamapps / "compatdata" / app.appid;
      app.is_native = !fs::is_directory(app.compat_data_dir, ec);
      if (!app.is_native) {
        if (auto info = ResolveProtonCompatInfo(app.compat_data_dir)) {
          app.proton_path = info->proton_path;
          app.client_install_path = info->client_install_path;
        }
      }

      apps.push_back(std::move(app));
    }
  }
  return apps;
}

std::map<std::string, AppActivity> ReadAppActivity(const fs::path& steam_root, std::string_view steamid64) {
  std::map<std::string, AppActivity> activity;
  const std::string account = steamid64.empty() ? LastSignedIn(steam_root) : std::string(steamid64);
  // userdata/ is keyed by the 32-bit account id, the SteamID64 minus its fixed individual-account base.
  constexpr std::uint64_t kSteamId64Base = 76561197960265728ULL;
  std::uint64_t id64 = 0;
  if (std::from_chars(account.data(), account.data() + account.size(), id64).ec != std::errc() ||
      id64 <= kSteamId64Base) {
    return activity;
  }
  const fs::path local_config =
      steam_root / "userdata" / std::to_string(id64 - kSteamId64Base) / "config" / "localconfig.vdf";
  const auto text = files::ReadFile(local_config);
  if (!text) return activity;
  const auto parsed = ParseVdf(*text);
  if (!parsed) return activity;
  const VdfValue* apps = parsed->Get({"UserLocalConfigStore", "Software", "Valve", "Steam", "apps"});
  if (apps == nullptr) return activity;
  for (const auto& [appid, app] : apps->children) {
    const AppActivity entry{.last_played_at = ParseInt64(app.Get({"LastPlayed"})),
                            .play_seconds = ParseInt64(app.Get({"Playtime"})) * 60};  // Steam stores minutes
    if (entry.last_played_at > 0 || entry.play_seconds > 0) activity[appid] = entry;
  }
  return activity;
}

}  // namespace mira::steam
