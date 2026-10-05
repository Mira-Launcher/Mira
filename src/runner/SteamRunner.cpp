#include "runner/SteamRunner.h"

#include <algorithm>
#include <filesystem>
#include <iterator>

#include "core/Strings.h"
#include "steam/SteamDetector.h"

namespace mira::runner {
namespace {
namespace fs = std::filesystem;
}  // namespace

std::vector<model::RunnerBuild> SteamRunner::Discover(const config::Config&) const {
  return {};  // no separate "builds" concept, see the class comment
}

Result<void> SteamRunner::Provision(const model::Game& game,
                                    const std::optional<model::RunnerBuild>&) const {
  // Steam already provisioned this prefix; Mira only has to confirm it's
  // still there. A native game (empty data_dir) needs nothing at all.
  if (game.data_dir.empty()) return {};
  std::error_code ec;
  if (!fs::is_directory(game.data_dir, ec)) {
    return Err("steam_prefix_missing",
              "this game's Steam-managed prefix is gone. Run it once through Steam first");
  }
  return {};
}

Result<Command> SteamRunner::BuildCommand(const model::Game& game,
                                          const std::optional<model::RunnerBuild>&) const {
  if (game.exe_path.empty()) {
    // Steam keeps a game's real launch command in its own appinfo cache, not on disk.
    return Err("no_executable", "Mira doesn't know which file starts this Steam game",
               "Choose the game's executable, or let Steam launch its games (Steam Launch Mode).",
               Fix::Game(game.id, "exe"));
  }

  const fs::path install_path = game.install_path;
  const fs::path exe = install_path / game.exe_path;

  Command command;

  if (game.data_dir.empty()) {
    // A native Steam game: no Proton involved, just exec it directly.
    command.argv = {exe.string()};
  } else {
    const auto info = steam::ResolveProtonCompatInfo(game.data_dir);
    if (!info) {
      return Err("steam_proton_unresolved",
                "couldn't determine which Proton build this prefix uses. Run this game once "
                "through Steam first");
    }
    command.argv = {info->proton_path.string(), "run"};
    std::ranges::move(WindowsProgram(exe), std::back_inserter(command.argv));
    command.env["STEAM_COMPAT_DATA_PATH"] = game.data_dir;
    command.env["STEAM_COMPAT_CLIENT_INSTALL_PATH"] = info->client_install_path.string();
  }

  for (const std::string& arg : strings::SplitArgs(game.args)) {
    if (!arg.empty()) command.argv.push_back(arg);
  }
  for (const auto& [key, value] : game.env) command.env[key] = value;  // game-specific wins

  command.cwd = game.working_dir.empty() ? exe.parent_path() : install_path / game.working_dir;
  return command;
}

}  // namespace mira::runner
