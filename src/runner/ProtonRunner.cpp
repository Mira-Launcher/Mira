#include "runner/ProtonRunner.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>

#include "config/Config.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "runner/Downloader.h"
#include "runner/Exec.h"
#include "runner/UmuDatabase.h"
#include "steam/SteamDetector.h"

namespace mira::runner {
namespace {
namespace fs = std::filesystem;

void ApplyGameId(Command& command, const config::Config& config, const model::Game& game) {
  const UmuMatch identity = UmuIdentity(config, game);
  command.env["GAMEID"] = identity.gameid;
  if (!identity.store.empty()) command.env["STORE"] = identity.store;
}

// Scanned on top of runner_search_paths (unless runner_scan_common_dirs is
// off), so builds from the distro, Steam, Heroic or ProtonPlus show up even
// with an old saved config.
constexpr const char* kKnownProtonDirs[] = {
    "~/.steam/root/compatibilitytools.d",          "~/.local/share/Steam/compatibilitytools.d",
    "~/.local/share/Steam/steamapps/common",       "/usr/share/steam/compatibilitytools.d",
    "/usr/local/share/steam/compatibilitytools.d", "~/.config/heroic/tools/proton",
};

// A distro package (/usr, /opt) or Steam's own Proton (steamapps/common).
bool UpdatedInPlace(const fs::path& dir) {
  const std::string path = dir.string();
  return path.starts_with("/usr/") || path.starts_with("/opt/") ||
         dir.parent_path().filename() == "common";
}

}  // namespace

fs::path BundledUmuRun() {
  return paths::UserDir() / "tools" / "umu" / "umu" / "umu-run";
}

std::string UmuRunPath() {
  if (auto found = FindOnPath("umu-run")) return *found;
  std::error_code ec;
  const fs::path bundled = BundledUmuRun();
  return fs::exists(bundled, ec) ? bundled.string() : std::string();
}

std::vector<model::RunnerBuild> ProtonRunner::Discover(const config::Config& config) const {
  std::vector<model::RunnerBuild> builds;
  // A Proton build is useless without the launcher that runs it.
  if (UmuRunPath().empty()) return builds;

  std::error_code ec;

  std::vector<fs::path> search_dirs = config.GetPathArray("runner_search_paths");
  if (config.GetBool("runner_scan_common_dirs")) {
    for (const char* dir : kKnownProtonDirs) search_dirs.push_back(paths::Expand(dir));
    // Steam's own Proton in every library, on any drive, and steam.root's tools.
    if (const auto root = steam::FindSteamRoot(config)) {
      search_dirs.push_back(*root / "compatibilitytools.d");
      for (const fs::path& library : steam::LibraryFolders(*root))
        search_dirs.push_back(library / "steamapps" / "common");
    }
  }

  for (const fs::path& search_dir : search_dirs) {
    if (!fs::is_directory(search_dir, ec)) continue;
    for (const auto& entry :
         fs::directory_iterator(search_dir, fs::directory_options::skip_permission_denied, ec)) {
      if (!entry.is_directory(ec)) continue;
      const fs::path& dir = entry.path();
      if (!fs::exists(dir / "proton", ec) || !fs::exists(dir / "toolmanifest.vdf", ec)) continue;
      // A build for the other architecture would be auto's pick and fail every launch.
      if (!ForThisMachine(dir.filename().string())) continue;

      model::RunnerBuild build;
      build.kind = "proton";
      build.path = dir.string();

      // The version file is "<unix timestamp> <name>", e.g.
      // "1789520217 GE-Proton11-7": timestamp for sorting "latest", name
      // for the human-facing "kind:name" reference.
      std::ifstream version_file(dir / "version");
      std::string line;
      if (version_file && std::getline(version_file, line)) {
        if (const auto space = line.find(' '); space != std::string::npos) {
          build.version = line.substr(0, space);
          build.release = strings::Trim(line.substr(space + 1));
        }
      }
      if (build.release.empty()) build.release = dir.filename().string();  // fallback
      // A distro package or Steam's own Proton is replaced in place by its
      // updates, and its release name changes with them; its folder doesn't.
      build.name = UpdatedInPlace(dir) ? dir.filename().string() : build.release;

      builds.push_back(std::move(build));
    }
  }
  return builds;
}

Result<void> ProtonRunner::Provision(const model::Game& game,
                                     const std::optional<model::RunnerBuild>& build) const {
  if (!build) return NoBuild("Proton");
  if (game.data_dir.empty()) return NoPrefix(game);

  std::error_code ec;
  fs::create_directories(game.data_dir, ec);
  if (ec) return PrefixCreateFailed(game, ec);

  // "" as the exe is umu's documented way to initialise a prefix with no
  // game to run (see `man umu`, Example 4).
  Command command;
  command.argv = {UmuRunPath(), ""};
  command.env["WINEPREFIX"] = game.data_dir;
  command.env["PROTONPATH"] = build->path;
  ApplyGameId(command, config_, game);

  auto result = RunAndWait(command);
  if (!result) return std::unexpected(result.error());

  // umu-run's exit code conflates "prefix init failed" with "no game to
  // launch". It's 1 even on a fully successful run here, since we
  // deliberately gave it nothing to launch. The only reliable success
  // signal is whether the prefix actually appeared on disk.
  if (!fs::exists(fs::path(game.data_dir) / "drive_c", ec)) {
    return Err(
        "provision_failed",
        std::format("umu-run produced no prefix (exit {}): {}", result->exit_code, result->output),
        "The Proton build may be broken. Try a different one.", Fix::Runners());
  }
  return {};
}

Result<Command> ProtonRunner::BuildCommand(const model::Game& game,
                                           const std::optional<model::RunnerBuild>& build) const {
  if (!build) return NoBuild("Proton");
  if (game.exe_path.empty()) return NoExecutable(game);
  if (game.data_dir.empty()) return NoPrefix(game);

  const fs::path exe = fs::path(game.install_path) / game.exe_path;

  Command command;
  command.argv = {UmuRunPath()};
  std::ranges::move(WindowsProgram(exe), std::back_inserter(command.argv));

  command.env["WINEPREFIX"] = game.data_dir;
  command.env["PROTONPATH"] = build->path;
  ApplyGameId(command, config_, game);
  ApplyGameLaunch(command, game, exe);
  return command;
}

nlohmann::json ProtonRunner::SettingsSchema() const {
  return nlohmann::json::array({
      {{"key", "gameid"},
       {"label", "umu game ID"},
       {"type", "string"},
       {"doc",
        "The GAMEID protonfixes looks this game's fixes up by, e.g. \"umu-397540\". Found in "
        "the umu database when not set; \"none\" turns that off."}},
      {{"key", "store"},
       {"label", "Store"},
       {"type", "string"},
       {"doc",
        "The STORE protonfixes looks this game's fixes up in, e.g. \"gog\", \"egs\" or "
        "\"ubisoft\". Set from the game's store when not set."}},
  });
}

}  // namespace mira::runner
