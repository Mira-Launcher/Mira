#pragma once

#include "runner/IRunner.h"

namespace mira::runner {

// Runs something through the exact Proton build and prefix Steam itself set
// up (game.data_dir is Steam's own compatdata/<appid>). The Proton build is
// resolved fresh from compat_data_dir/config_info at BuildCommand time, so
// there's no Discover() and no build to pick.
//
// Used for steam.launch_mode == "direct" and for running an exe inside a
// Steam-owned prefix. launch_mode == "steam" (the default) never reaches
// this class, because that mode hands off to steam -applaunch <appid> instead.
class SteamRunner : public IRunner {
public:
  std::string kind() const override { return "steam"; }

  std::vector<model::RunnerBuild> Discover(const config::Config& config) const override;
  bool UsesBuilds() const override { return false; }

  Result<void> Provision(const model::Game& game,
                         const std::optional<model::RunnerBuild>& build) const override;
  Result<Command> BuildCommand(const model::Game& game,
                               const std::optional<model::RunnerBuild>& build) const override;
};

}  // namespace mira::runner
