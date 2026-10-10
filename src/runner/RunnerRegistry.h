#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "config/Config.h"
#include "core/Command.h"
#include "core/Result.h"
#include "model/Types.h"
#include "runner/IRunner.h"

namespace mira::runner {

// Collapses builds that are the same runner found more than once, the same
// directory reached through a symlinked search path, or two entries sharing
// a "kind:name" reference, which no client can tell apart. Order is
// preserved and the first occurrence wins. Exposed for its own test; every
// discovery goes through it.
std::vector<model::RunnerBuild> DeduplicateBuilds(std::vector<model::RunnerBuild> builds);

// What "<kind>:auto" picks from `builds` (non-empty): a distro-packaged build
// first, then the preferred download source (GE-Proton, Wine staging-tkg),
// then the newest.
const model::RunnerBuild& PickAuto(const config::Config& config, const std::vector<model::RunnerBuild>& builds);

// Owns every IRunner and resolves a "kind:name" reference (name may be
// "auto"/"latest" for the newest discovered build) to a concrete runner +
// build. The one place a game's runner_ref gets turned into actual work.
// Held while something provisions or runs winetricks in a prefix, so two never touch one prefix at once.
std::unique_lock<std::mutex> LockPrefix(const std::string& data_dir);

class RunnerRegistry {
public:
  explicit RunnerRegistry(config::Config& config);

  std::vector<model::RunnerBuild> DiscoverAll() const;

  // Looks up a runner by kind alone, with no build resolution, for
  // anything that only needs the runner itself (e.g. its SettingsSchema()),
  // not a "kind:name" reference. nullptr if no runner of that kind exists.
  const IRunner* FindByKind(const std::string& kind) const;

  struct Resolved {
    const IRunner* runner;
    std::optional<model::RunnerBuild> build;  // unset for a runner with no builds (native)
  };
  Result<Resolved> Resolve(const std::string& runner_ref) const;

  // Turns game.runner_ref into a concrete "kind:name" string, without
  // provisioning anything: empty -> default_runner.<platform>, "auto" ->
  // best available windows runner. Used by launch paths that must not
  // re-run a runner's Provision() (wineboot, prefix init, ...) on every
  // launch the way ProvisionGame does -- only ProvisionGame's own callers
  // (install/scan/finish-install) should provision.
  std::string ResolveRef(const model::Game& game) const;

  // Resolves game.runner_ref (or, if unset, default_runner.<platform>),
  // pins the concrete reference onto the returned copy, and provisions it.
  // Can't fail in the Result sense: "no runner found" or "provisioning
  // failed" are both normal outcomes, reflected as status=broken with
  // last_error set, same as AutoSetup does for "no executable found".
  // Doesn't persist anything; the caller saves the result via GameStore.
  model::Game ProvisionGame(model::Game game) const;

private:
  // Discovered builds for one kind, computed once per registry instance.
  const std::vector<model::RunnerBuild>& BuildsFor(const std::string& kind) const;

  config::Config& config_;
  std::map<std::string, std::unique_ptr<IRunner>> runners_;

  mutable std::mutex cache_mutex_;
  mutable std::map<std::string, std::vector<model::RunnerBuild>> cache_;
};

}  // namespace mira::runner
