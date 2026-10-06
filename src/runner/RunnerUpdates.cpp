#include "runner/RunnerUpdates.h"

#include <algorithm>
#include <format>

#include "core/Paths.h"

namespace mira::runner {

std::filesystem::path BuildDir(const model::RunnerBuild& build) {
  const std::filesystem::path path(build.path);
  return build.kind == "wine" ? path.parent_path().parent_path() : path;
}

std::vector<std::filesystem::path> RunnerRoots(const config::Config& config, const std::string& kind) {
  return config.GetPathArray(kind == "wine" ? "wine_search_paths" : "runner_search_paths");
}

Result<RunnerFamily> FamilyFor(const config::Config& config, const std::string& kind, const std::string& source) {
  if (source.empty()) {
    const auto families = Families(config, kind);
    if (families.empty()) return Err("unknown_runner_kind", std::format("nothing to download for \"{}\"", kind));
    return families.front();
  }
  auto family = FindFamily(config, source);
  if (!family || family->kind != kind) {
    return Err("unknown_runner_source", std::format("no {} source \"{}\"", kind, source));
  }
  return *family;
}

std::vector<model::RunnerBuild> BuildsOfKind(const RunnerRegistry& registry, const std::string& kind) {
  std::vector<model::RunnerBuild> out = registry.DiscoverAll();
  std::erase_if(out, [&](const model::RunnerBuild& build) { return build.kind != kind; });
  return out;
}

bool HasInstalled(const std::vector<model::RunnerBuild>& builds, const ReleaseAsset& release) {
  return std::ranges::any_of(builds, [&](const model::RunnerBuild& build) {
    return IsInstalledAs(build.kind, build.release, BuildDir(build).filename().string(), release);
  });
}

std::vector<RunnerUpdate> FindRunnerUpdates(const config::Config& config, const RunnerRegistry& registry) {
  std::vector<RunnerUpdate> out;
  for (const std::string kind : {"proton", "wine"}) {
    const std::vector<model::RunnerBuild> builds = BuildsOfKind(registry, kind);
    for (const model::RunnerBuild& build : builds) {
      const std::filesystem::path dir = BuildDir(build);
      if (!paths::IsWithin(dir, RunnerRoots(config, kind))) continue;
      auto family = FamilyOfBuild(config, kind, build.release, dir.filename().string());
      if (!family) continue;
      auto releases = ListFamilyReleases(*family);
      if (!releases || releases->empty() || HasInstalled(builds, releases->front())) continue;
      out.push_back({build, std::move(*family), releases->front()});
    }
  }
  return out;
}

}  // namespace mira::runner
