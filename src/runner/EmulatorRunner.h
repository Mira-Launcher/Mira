#pragma once

#include "runner/IRunner.h"

namespace mira::emulators {
struct Emulator;
}

namespace mira::runner {

// Runs `program` as the emulator: its prefill files written, its environment, and an AppImage working
// without FUSE.
Command EmulatorCommand(const std::filesystem::path& program, const emulators::Emulator& emulator);

// Runs an emulator's game: the emulator (emulators::Binary) with its args and the game's file.
class EmulatorRunner final : public IRunner {
public:
  explicit EmulatorRunner(const config::Config& config) : config_(config) {}
  std::string kind() const override { return "emulator"; }
  bool UsesBuilds() const override { return false; }
  std::vector<model::RunnerBuild> Discover(const config::Config&) const override { return {}; }
  Result<void> Provision(const model::Game&, const std::optional<model::RunnerBuild>&) const override {
    return {};
  }
  Result<Command> BuildCommand(const model::Game& game,
                               const std::optional<model::RunnerBuild>&) const override;

private:
  const config::Config& config_;
};

}  // namespace mira::runner
