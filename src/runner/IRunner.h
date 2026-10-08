#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <json.hpp>

#include "core/Command.h"
#include "core/Result.h"
#include "model/Types.h"

namespace mira::config {
class Config;
}

namespace mira::runner {

// What Wine runs for a Windows file: an .msi goes through msiexec and a
// .bat/.cmd through cmd, everything else runs as is.
std::vector<std::string> WindowsProgram(const std::filesystem::path& file);

// The errors every runner shares, worded the same everywhere.
std::unexpected<Error> NoBuild(std::string_view kind);  // "Wine" or "Proton"
std::unexpected<Error> NoExecutable(const model::Game& game);
std::unexpected<Error> NoPrefix(const model::Game& game);
std::unexpected<Error> PrefixCreateFailed(const model::Game& game, const std::error_code& ec);

// The part of a launch every runner shares: the game's own args after the
// program, its env on top of the runner's, and its working directory.
void ApplyGameLaunch(Command& command, const model::Game& game, const std::filesystem::path& exe);

// One way to run a game: native, Proton, Wine or Steam. Every umu/Proton
// detail lives inside ProtonRunner; this interface knows none of it.
class IRunner {
public:
  virtual ~IRunner() = default;

  virtual std::string kind() const = 0;

  // Installed builds of this kind (Proton versions, for umu). Empty for a
  // runner with no such concept (native).
  virtual std::vector<model::RunnerBuild> Discover(const config::Config& config) const = 0;

  // False for a runner with no concept of separate installed builds. Without
  // this, "discovery returned nothing" and "this kind has no builds" are
  // indistinguishable, and a reference naming an uninstalled build resolves
  // as success-with-no-build instead of a clear error.
  virtual bool UsesBuilds() const { return true; }

  // Sets up whatever the game needs before it can launch (a Wine prefix).
  // No-op for runners that need nothing.
  virtual Result<void> Provision(const model::Game& game,
                                 const std::optional<model::RunnerBuild>& build) const = 0;

  virtual Result<Command> BuildCommand(const model::Game& game,
                                       const std::optional<model::RunnerBuild>& build) const = 0;

  // Declares what game.runner_config accepts for this kind, so a frontend
  // can render it generically. Empty for a runner with no runner_config
  // fields, which is most of them.
  virtual nlohmann::json SettingsSchema() const { return nlohmann::json::array(); }
};

}  // namespace mira::runner
