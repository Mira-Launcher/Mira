#include "runner/RunnerRegistry.h"

#include <algorithm>
#include <filesystem>
#include <charconv>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <system_error>
#include <tuple>

#include "config/Schema.h"
#include "core/Log.h"
#include "runner/Downloader.h"
#include "runner/NativeRunner.h"
#include "runner/ProtonRunner.h"
#include "runner/SteamRunner.h"
#include "runner/WineRunner.h"

namespace mira::runner {
namespace {

// Orders builds of one runner kind for "latest". Proton's version field is a
// bare unix timestamp; Wine's is a string like "wine-10.0 (Staging)", where
// parsing from the front yields nothing at all and made every build compare
// equal, so scan to the first digit and read up to two dotted components.
// Only ever compares builds of the same kind, so the differing scales between
// kinds don't matter.
std::int64_t ParseVersion(const std::string& version) {
  size_t i = version.find_first_of("0123456789");
  if (i == std::string::npos) return 0;

  std::int64_t major = 0;
  const auto* begin = version.data() + i;
  const auto* end = version.data() + version.size();
  const auto first = std::from_chars(begin, end, major);
  if (first.ec != std::errc{}) return 0;

  std::int64_t minor = 0;
  if (first.ptr != end && *first.ptr == '.') {
    std::from_chars(first.ptr + 1, end, minor);
  }
  return major * 1000 + minor;
}

// "auto"'s order: a build the distro packages (kept current by the package
// manager, and tuned for that system), then Mira's preferred families, then
// the newest.
std::tuple<bool, int, std::int64_t> AutoRank(const config::Config& config, const model::RunnerBuild& build) {
  const bool packaged = build.path.starts_with("/usr/") || build.path.starts_with("/opt/");
  const std::vector<RunnerFamily> families = Families(config, build.kind);
  const std::filesystem::path dir =
      build.kind == "wine" ? std::filesystem::path(build.path).parent_path().parent_path() : std::filesystem::path(build.path);
  int preference = 0;
  if (auto family = FamilyOfBuild(config, build.kind, build.release, dir.filename().string())) {
    const auto at = std::ranges::find(families, family->id, &RunnerFamily::id);
    preference = static_cast<int>(families.end() - at);
  }
  return {packaged, preference, ParseVersion(build.version)};
}

}  // namespace

RunnerRegistry::RunnerRegistry(config::Config& config) : config_(config) {
  auto native = std::make_unique<NativeRunner>();
  auto proton = std::make_unique<ProtonRunner>();
  auto wine = std::make_unique<WineRunner>();
  auto steam = std::make_unique<SteamRunner>();
  runners_[native->kind()] = std::move(native);
  runners_[proton->kind()] = std::move(proton);
  runners_[wine->kind()] = std::move(wine);
  runners_[steam->kind()] = std::move(steam);
}

// One build, discovered once. On a normal Arch/Steam setup `~/.steam/steam`
// symlinks to `~/.local/share/Steam`, which `libraryfolders.vdf` also lists,
// so the same Proton build gets found twice under different `path`s but the
// same `kind:name` reference. Dedupe on resolved path first (the actual
// cause), then on reference (two builds sharing one are indistinguishable
// to any client).
std::vector<model::RunnerBuild> DeduplicateBuilds(std::vector<model::RunnerBuild> builds) {
  std::vector<model::RunnerBuild> unique;
  std::set<std::string> seen_paths;
  std::set<std::string> seen_references;

  for (model::RunnerBuild& build : builds) {
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(build.path, ec);
    const std::string path_key = ec ? build.path : resolved.string();
    if (!build.path.empty() && !seen_paths.insert(path_key).second) continue;
    if (!seen_references.insert(build.Reference()).second) continue;
    unique.push_back(std::move(build));
  }
  return unique;
}

const model::RunnerBuild& PickAuto(const config::Config& config, const std::vector<model::RunnerBuild>& builds) {
  return *std::ranges::max_element(builds, {}, [&](const model::RunnerBuild& build) { return AutoRank(config, build); });
}

const std::vector<model::RunnerBuild>& RunnerRegistry::BuildsFor(const std::string& kind) const {
  // Discovery is not free: WineRunner spawns `wine --version` per build,
  // and one scan resolves a runner for every new game it finds. A registry
  // is constructed per scan/request, so caching for its lifetime removes the
  // repeated cost without ever going stale in practice.
  std::lock_guard lock(cache_mutex_);
  if (auto it = cache_.find(kind); it != cache_.end()) return it->second;
  const auto runner = runners_.find(kind);
  if (runner == runners_.end()) return cache_[kind];  // empty
  return cache_[kind] = DeduplicateBuilds(runner->second->Discover(config_));
}

std::vector<model::RunnerBuild> RunnerRegistry::DiscoverAll() const {
  std::vector<model::RunnerBuild> all;
  for (const auto& [kind, runner] : runners_) {
    const std::vector<model::RunnerBuild>& found = BuildsFor(kind);
    all.insert(all.end(), found.begin(), found.end());
  }
  return all;
}

const IRunner* RunnerRegistry::FindByKind(const std::string& kind) const {
  const auto it = runners_.find(kind);
  return it == runners_.end() ? nullptr : it->second.get();
}

Result<RunnerRegistry::Resolved> RunnerRegistry::Resolve(const std::string& runner_ref) const {
  const auto colon = runner_ref.find(':');
  if (colon == std::string::npos) {
    return Err("invalid_runner_ref", std::format("\"{}\" is not \"kind:name\"", runner_ref));
  }
  std::string kind = runner_ref.substr(0, colon);
  // umu used to be modelled as its own runner kind. It's the mechanism
  // Proton runs through, not a runner. Accept the old spelling so a
  // games.toml written before the rename keeps resolving.
  if (kind == "proton_umu") kind = "proton";
  const std::string name = runner_ref.substr(colon + 1);

  const auto runner_it = runners_.find(kind);
  if (runner_it == runners_.end()) {
    return Err("unknown_runner_kind", std::format("no runner of kind \"{}\"", kind));
  }
  const IRunner* runner = runner_it->second.get();
  if (!runner->UsesBuilds()) return Resolved{runner, std::nullopt};  // native

  const std::vector<model::RunnerBuild>& builds = BuildsFor(kind);
  if (builds.empty()) {
    return Err("runner_build_not_found", std::format("no {} builds are installed", kind),
               std::format("Install a {} build.", kind), Fix::Runners());
  }

  if (name == "auto") return Resolved{runner, PickAuto(config_, builds)};
  if (name == "latest") {
    auto newest = std::ranges::max_element(builds, {}, [](const model::RunnerBuild& build) {
      return ParseVersion(build.version);
    });
    return Resolved{runner, *newest};
  }

  const auto match = std::ranges::find(builds, name, &model::RunnerBuild::name);
  if (match == builds.end()) {
    return Err("runner_build_not_found", std::format("the {} build \"{}\" isn't installed anymore", kind, name),
               "Install it again, or choose another runner.", Fix::Runners());
  }
  return Resolved{runner, *match};
}

std::string RunnerRegistry::ResolveRef(const model::Game& game) const {
  std::string ref = game.runner_ref;
  // Stores with a <source>.runner key can set their own default.
  if (ref.empty() && game.platform != model::Platform::Native && !game.source.empty() &&
      config::Schema::Instance().Find(game.source + ".runner") != nullptr) {
    ref = config_.GetString(game.source + ".runner");
  }
  if (ref.empty()) {
    ref = config_.GetString(game.platform == model::Platform::Native ? "default_runner.native"
                                                                     : "default_runner.windows");
  }
  // "auto" isn't itself "kind:name": it means "the best available windows
  // runner": Proton if a build is installed (protonfixes come with it),
  // else plain Wine, else nothing usable and Resolve below says so clearly.
  if (ref == "auto") ref = BuildsFor("proton").empty() ? "wine:auto" : "proton:auto";
  return ref;
}

std::unique_lock<std::mutex> LockPrefix(const std::string& data_dir) {
  static std::mutex map_mutex;
  static std::map<std::string, std::unique_ptr<std::mutex>> locks;
  std::mutex* lock = nullptr;
  {
    const std::lock_guard guard(map_mutex);
    auto& entry = locks[data_dir];
    if (!entry) entry = std::make_unique<std::mutex>();
    lock = entry.get();
  }
  return std::unique_lock(*lock);
}

model::Game RunnerRegistry::ProvisionGame(model::Game game) const {
  const std::string ref = ResolveRef(game);
  const Result<Resolved> resolved = Resolve(ref);
  if (!resolved) {
    game.status = model::GameStatus::Broken;
    game.last_error = resolved.error().message;
    return game;
  }

  // Pin the concrete build, not "latest". A Proton update afterwards must
  // not silently change a working game's runtime.
  game.runner_ref = resolved->build ? std::format("{}:{}", resolved->runner->kind(), resolved->build->name)
                                    : ref;

  std::unique_lock<std::mutex> prefix_lock;
  if (!game.data_dir.empty()) prefix_lock = LockPrefix(game.data_dir);
  const Result<void> provisioned = resolved->runner->Provision(game, resolved->build);
  if (!provisioned) {
    game.status = model::GameStatus::Broken;
    game.last_error = provisioned.error().message;
    log::Warn("provisioning {} failed: {}", game.id, provisioned.error().message);
  } else {
    game.status = model::GameStatus::Ready;
    game.last_error.clear();
  }
  return game;
}

}  // namespace mira::runner
