#pragma once

#include "runner/IRunner.h"

namespace mira::runner {

// Runs Windows games through Wine, without Proton or umu.
class WineRunner final : public IRunner {
 public:
  std::string kind() const override { return "wine"; }
  std::vector<model::RunnerBuild> Discover(const config::Config& config) const override;
  Result<void> Provision(const model::Game& game,
                         const std::optional<model::RunnerBuild>& build) const override;
  Result<Command> BuildCommand(const model::Game& game,
                               const std::optional<model::RunnerBuild>& build) const override;
};

}  // namespace mira::runner
