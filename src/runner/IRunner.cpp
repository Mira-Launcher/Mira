#include "runner/IRunner.h"

#include <format>

#include "core/Strings.h"

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

void ApplyGameLaunch(Command& command, const model::Game& game, const std::filesystem::path& exe) {
  for (const std::string& arg : strings::SplitArgs(game.args)) {
    if (!arg.empty()) command.argv.push_back(arg);
  }
  for (const auto& [key, value] : game.env) command.env[key] = value;  // game-specific wins
  command.cwd = game.working_dir.empty() ? exe.parent_path()
                                         : std::filesystem::path(game.install_path) / game.working_dir;
}

std::vector<std::string> WindowsProgram(const std::filesystem::path& file) {
  const std::string ext = strings::ToLower(file.extension().string());
  if (ext == ".msi") return {"msiexec", "/i", file.string()};
  if (ext == ".bat" || ext == ".cmd") return {"cmd", "/c", file.string()};
  return {file.string()};
}

}  // namespace mira::runner
