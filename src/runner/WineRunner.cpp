#include "runner/WineRunner.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <iterator>

#include "config/Config.h"
#include "core/Paths.h"
#include "core/Strings.h"
#include "runner/Exec.h"

namespace mira::runner {

std::unexpected<Error> NoBuild(std::string_view kind) {
  return Err("no_runner_build", std::format("no {} build is installed for this game to run with", kind),
             std::format("Install a {} build, or choose another runner for the game.", kind), Fix::Runners());
}

std::unexpected<Error> NoExecutable(const model::Game& game) {
  return Err("no_executable", "this game has no executable set", "Choose the file that starts the game.",
             Fix::Game(game.id, "exe"));
}

std::unexpected<Error> NoPrefix(const model::Game& game) {
  return Err("no_data_dir", "this game has no folder for its Wine prefix",
             "Set a data directory for the game, where its Wine prefix will go.", Fix::Game(game.id, "data_dir"));
}

std::unexpected<Error> PrefixCreateFailed(const model::Game& game, const std::error_code& ec) {
  return Err("prefix_create_failed", std::format("couldn't create the Wine prefix at {}: {}", game.data_dir, ec.message()),
             "Check that the prefix folder is writable, or choose another one.", Fix::Setting("prefix_root"));
}

std::vector<std::string> WindowsProgram(const std::filesystem::path& file) {
  const std::string ext = strings::ToLower(file.extension().string());
  if (ext == ".msi") return {"msiexec", "/i", file.string()};
  if (ext == ".bat" || ext == ".cmd") return {"cmd", "/c", file.string()};
  return {file.string()};
}

namespace {
namespace fs = std::filesystem;

// "wine wineboot" (rather than a separate wineboot binary) works uniformly
// for the system wine and any custom build found under wine_search_paths:
// every full Wine install has an internal wineboot component reachable this
// way, whether or not a standalone `wineboot` binary sits next to it.
// Scanned on top of wine_search_paths unless runner_scan_common_dirs is off. /opt holds distro builds such as
// wine-cachyos-opt's /opt/wine-cachyos.
constexpr const char* kKnownWineDirs[] = {
    "~/.local/share/lutris/runners/wine",
    "~/.config/heroic/tools/wine",
    "~/.local/share/bottles/runners",
    "/opt",
};

}  // namespace

std::vector<model::RunnerBuild> WineRunner::Discover(const config::Config& config) const {
  std::vector<model::RunnerBuild> builds;

  if (auto system_wine = FindOnPath("wine")) {
    builds.push_back({.kind = "wine", .name = "system", .path = *system_wine,
                      .version = ToolVersion(*system_wine), .release = "system"});
  }

  std::vector<fs::path> search_dirs = config.GetPathArray("wine_search_paths");
  if (config.GetBool("runner_scan_common_dirs")) {
    for (const char* dir : kKnownWineDirs) search_dirs.push_back(paths::Expand(dir));
  }

  std::error_code ec;
  for (const fs::path& search_dir : search_dirs) {
    if (!fs::is_directory(search_dir, ec)) continue;
    for (const auto& entry : fs::directory_iterator(search_dir, fs::directory_options::skip_permission_denied, ec)) {
      if (!entry.is_directory(ec)) continue;
      const fs::path wine_binary = entry.path() / "bin" / "wine";
      if (!fs::exists(wine_binary, ec)) continue;
      const std::string name = entry.path().filename().string();
      builds.push_back({.kind = "wine", .name = name, .path = wine_binary.string(),
                        .version = ToolVersion(wine_binary.string()), .release = name});
    }
  }
  return builds;
}

Result<void> WineRunner::Provision(const model::Game& game,
                                   const std::optional<model::RunnerBuild>& build) const {
  if (!build) return NoBuild("Wine");
  if (game.data_dir.empty()) return NoPrefix(game);

  std::error_code ec;
  fs::create_directories(game.data_dir, ec);
  if (ec) return PrefixCreateFailed(game, ec);

  Command command;
  command.argv = {build->path, "wineboot", "-u"};
  command.env["WINEPREFIX"] = game.data_dir;
  // Provisioning runs unattended, off a background scan the user never
  // asked for -- a Gecko/Mono install prompt popping up and waiting for a
  // click would hang it indefinitely (and pop a real window on the user's
  // desktop for a prefix they didn't know was being created). Neither DLL
  // is needed for a plain wineboot init.
  command.env["WINEDLLOVERRIDES"] = "mscoree,mshtml=";
  command.timeout_s = 300;

  auto result = RunAndWait(command);
  if (!result) return std::unexpected(result.error());

  // Observed directly: `wine wineboot` reports exit 0 even when it failed
  // outright (e.g. WINEPREFIX not pre-created), the only reliable signal,
  // same as ProtonRunner, is whether the prefix actually appeared on disk.
  if (!fs::exists(fs::path(game.data_dir) / "drive_c", ec)) {
    return Err("provision_failed",
              std::format("wineboot produced no prefix (exit {}): {}", result->exit_code, result->output),
              "The Wine build may be broken. Try a different one.", Fix::Runners());
  }
  return {};
}

Result<Command> WineRunner::BuildCommand(const model::Game& game,
                                         const std::optional<model::RunnerBuild>& build) const {
  if (!build) return NoBuild("Wine");
  if (game.exe_path.empty()) return NoExecutable(game);
  if (game.data_dir.empty()) return NoPrefix(game);

  const fs::path install_path = game.install_path;
  const fs::path exe = install_path / game.exe_path;

  Command command;
  command.argv = {build->path};
  std::ranges::move(WindowsProgram(exe), std::back_inserter(command.argv));
  for (const std::string& arg : strings::SplitArgs(game.args)) {
    if (!arg.empty()) command.argv.push_back(arg);
  }

  command.env["WINEPREFIX"] = game.data_dir;
  for (const auto& [key, value] : game.env) command.env[key] = value;  // game-specific wins

  command.cwd = game.working_dir.empty() ? exe.parent_path() : install_path / game.working_dir;
  return command;
}

}  // namespace mira::runner
