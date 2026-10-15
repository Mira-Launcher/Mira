#pragma once

#include <filesystem>

#include "runner/IRunner.h"

namespace mira::runner {

// Runs Windows games through Proton, any build: Proton-GE, Proton-CachyOS,
// UMU-Proton, Valve's own.
//
// umu-run is the *mechanism*, not the identity: Proton outside Steam needs
// the Steam Linux Runtime container and a pile of STEAM_COMPAT_* env that
// umu supplies, plus protonfixes for free. Running `proton run` directly is
// possible and loses both, so Proton always runs through umu.
// umu-run on PATH, else the copy Mira installed; empty if neither.
std::string UmuRunPath();
// Where Mira installs umu-launcher's own copy of umu-run.
std::filesystem::path BundledUmuRun();

class ProtonRunner final : public IRunner {
 public:
  std::string kind() const override { return "proton"; }
  std::vector<model::RunnerBuild> Discover(const config::Config& config) const override;
  Result<void> Provision(const model::Game& game,
                         const std::optional<model::RunnerBuild>& build) const override;
  Result<Command> BuildCommand(const model::Game& game,
                               const std::optional<model::RunnerBuild>& build) const override;
  nlohmann::json SettingsSchema() const override;
};

}  // namespace mira::runner
