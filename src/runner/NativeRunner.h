#pragma once

#include "runner/IRunner.h"

namespace mira::runner {

// Runs a native Linux binary directly. No prefix, no versions to choose
// between -- Discover() still reports one synthetic "native:native" entry
// (not an empty list) so it shows up at all in GET /v1/runners: a runner
// picker built purely from discovered builds otherwise never offers
// "native" as a choice, with no way to correct a game that detected as the
// wrong platform (e.g. an .exe picked over the .sh actually meant to run).
class NativeRunner final : public IRunner {
public:
  std::string kind() const override { return "native"; }
  bool UsesBuilds() const override { return false; }
  std::vector<model::RunnerBuild> Discover(const config::Config&) const override {
    return {model::RunnerBuild{.kind = "native", .name = "native", .path = "", .version = "", .release = "native"}};
  }
  Result<void> Provision(const model::Game&, const std::optional<model::RunnerBuild>&) const override {
    return {};
  }
  Result<Command> BuildCommand(const model::Game& game,
                               const std::optional<model::RunnerBuild>&) const override;
};

}  // namespace mira::runner
