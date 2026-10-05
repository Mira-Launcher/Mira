#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"
#include "runner/Downloader.h"
#include "runner/RunnerRegistry.h"

namespace mira::runner {

// A build's own directory. Wine's path is <dir>/bin/wine.
std::filesystem::path BuildDir(const model::RunnerBuild& build);

// The folders Mira installs builds of `kind` into, and may delete them from.
std::vector<std::filesystem::path> RunnerRoots(const config::Config& config, const std::string& kind);

// `source`, or the kind's preferred source when empty.
Result<RunnerFamily> FamilyFor(const config::Config& config, const std::string& kind, const std::string& source);

std::vector<model::RunnerBuild> BuildsOfKind(const RunnerRegistry& registry, const std::string& kind);

bool HasInstalled(const std::vector<model::RunnerBuild>& builds, const ReleaseAsset& release);

struct RunnerUpdate {
  model::RunnerBuild build;
  RunnerFamily family;
  ReleaseAsset latest;
};

// Removable builds whose source's newest release isn't installed yet.
std::vector<RunnerUpdate> FindRunnerUpdates(const config::Config& config, const RunnerRegistry& registry);

}  // namespace mira::runner
