#include "runner/EmulatorRunner.h"

#include <format>

#include "core/Strings.h"
#include "emulators/Emulators.h"
#include "runner/NativeRunner.h"

namespace mira::runner {

Result<Command> EmulatorRunner::BuildCommand(const model::Game& game,
                                             const std::optional<model::RunnerBuild>&) const {
  const emulators::Emulator* emulator = emulators::ForGame(game);
  if (emulator == nullptr) {
    return Err("not_emulator_game", std::format("{} isn't one of an emulator's games", game.name));
  }
  const std::filesystem::path binary = emulators::Binary(config_, *emulator);
  if (binary.empty()) {
    return Err("emulator_missing", std::format("{} isn't installed", emulator->name),
               std::format("Install {} from its source page.", emulator->name));
  }
  const std::filesystem::path rom = std::filesystem::path(game.install_path) / game.exe_path;

  Command command;
  command.argv.push_back(binary.string());
  if (strings::ToLower(binary.extension().string()) == ".appimage" && !HasFuse())
    command.argv.push_back("--appimage-extract-and-run");
  // The game's own args are the emulator's options, so they go before its file.
  for (const std::string& arg : strings::SplitArgs(game.args)) {
    if (!arg.empty()) command.argv.push_back(arg);
  }
  for (const std::string& arg : emulator->args) command.argv.push_back(arg == "{rom}" ? rom.string() : arg);
  for (const auto& [key, value] : game.env) command.env[key] = value;
  command.cwd = rom.parent_path();
  return command;
}

}  // namespace mira::runner
