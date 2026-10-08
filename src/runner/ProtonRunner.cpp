#include "runner/ProtonRunner.h"

#include <cctype>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <format>
#include <fstream>

#include "config/Config.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "runner/Exec.h"

namespace mira::runner {
namespace {
namespace fs = std::filesystem;

// Without a GAMEID every game runs as umu-default, so Proton gives all of
// their windows the same class (steam_app_default) and desktops group them
// as one app. umu only accepts letters, digits and underscores after "umu-".
void ApplyGameId(Command& command, const model::Game& game) {
  if (auto it = game.runner_config.find("gameid"); it != game.runner_config.end() && it->is_string()) {
    command.env["GAMEID"] = it->get<std::string>();
  } else {
    std::string id = "umu-mira_";
    for (const char ch : game.id) {
      if (std::isalnum(static_cast<unsigned char>(ch))) id.push_back(ch);
    }
    command.env["GAMEID"] = id;
  }
  if (auto it = game.runner_config.find("store"); it != game.runner_config.end() && it->is_string()) {
    command.env["STORE"] = it->get<std::string>();
  }
}

// Scanned on top of runner_search_paths (unless runner_scan_common_dirs is
// off), so builds from the distro, Steam, Heroic or ProtonPlus show up even
// with an old saved config.
constexpr const char* kKnownProtonDirs[] = {
    "~/.steam/root/compatibilitytools.d",
    "~/.local/share/Steam/compatibilitytools.d",
    "~/.local/share/Steam/steamapps/common",
    "/usr/share/steam/compatibilitytools.d",
    "/usr/local/share/steam/compatibilitytools.d",
    "~/.config/heroic/tools/proton",
};

// A distro package (/usr, /opt) or Steam's own Proton (steamapps/common).
bool UpdatedInPlace(const fs::path& dir) {
  const std::string path = dir.string();
  return path.starts_with("/usr/") || path.starts_with("/opt/") || dir.parent_path().filename() == "common";
}

}  // namespace

fs::path BundledUmuRun() { return paths::UserDir() / "tools" / "umu" / "umu" / "umu-run"; }

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
  }

  for (const fs::path& search_dir : search_dirs) {
    if (!fs::is_directory(search_dir, ec)) continue;
    for (const auto& entry : fs::directory_iterator(search_dir, fs::directory_options::skip_permission_denied, ec)) {
      if (!entry.is_directory(ec)) continue;
      const fs::path& dir = entry.path();
      if (!fs::exists(dir / "proton", ec) || !fs::exists(dir / "toolmanifest.vdf", ec)) continue;

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
  ApplyGameId(command, game);

  auto result = RunAndWait(command);
  if (!result) return std::unexpected(result.error());

  // umu-run's exit code conflates "prefix init failed" with "no game to
  // launch". It's 1 even on a fully successful run here, since we
  // deliberately gave it nothing to launch. The only reliable success
  // signal is whether the prefix actually appeared on disk.
  if (!fs::exists(fs::path(game.data_dir) / "drive_c", ec)) {
    return Err("provision_failed",
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
  ApplyGameId(command, game);
  ApplyGameLaunch(command, game, exe);
  return command;
}

nlohmann::json ProtonRunner::SettingsSchema() const {
  return nlohmann::json::array({
      {{"key", "gameid"},
       {"label", "Steam game ID"},
       {"type", "string"},
       {"doc", "Steam AppID umu reports through the GAMEID environment variable. It decides which "
               "protonfixes entry Proton applies. Optional: without it umu uses a generic "
               "default (\"umu-default\")."}},
      {{"key", "store"},
       {"label", "Store"},
       {"type", "string"},
       {"doc", "Store umu should report via the STORE env var (e.g. \"ubisoft\", \"battlenet\", \"ea\"), "
               "so store-specific protonfixes apply. Set automatically for store launcher games."}},
  });
}

}  // namespace mira::runner
