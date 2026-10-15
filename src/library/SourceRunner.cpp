#include "library/SourceRunner.h"

#include <format>
#include <functional>

#include "config/Schema.h"
#include "core/StoreErrors.h"
#include "launchers/Launchers.h"
#include "runner/RunnerRegistry.h"

namespace mira::library {
namespace {

std::string StoreKey(std::string_view source) {
  return std::format("{}.runner", source);
}

bool HasStoreKey(std::string_view source) {
  return config::Schema::Instance().Find(StoreKey(source)) != nullptr;
}

bool IsSourceGame(const model::Game& game, std::string_view source) {
  return game.source == source && game.platform != model::Platform::Native;
}

// Launcher games share the launcher's prefix; one moved elsewhere keeps its own runner.
bool IsLauncherGame(const model::Game& game, const model::Game& host) {
  return game.id == host.id || (game.source == host.source_ref && game.data_dir == host.data_dir);
}

Result<model::Game> LauncherHost(const store::GameStore& games, std::string_view source) {
  const launchers::Launcher* launcher = launchers::Find(source);
  if (launcher == nullptr) {
    return Err("no_runner", std::format("{} has no runner of its own", source));
  }
  const auto host = games.Find(launchers::GameId(*launcher));
  if (!host || host->data_dir.empty()) {
    return LauncherNotInstalled(launcher->id, launcher->name);
  }
  return *host;
}

}  // namespace

Result<SourceRunner> GetSourceRunner(const config::Config& config, const store::GameStore& games,
                                     std::string_view source) {
  SourceRunner out;
  if (HasStoreKey(source)) {
    out.runner_ref = config.GetString(StoreKey(source));
    for (const model::Game& game : games.All()) {
      if (!IsSourceGame(game, source)) continue;
      ++out.games;
      if (game.runner_ref != out.runner_ref) ++out.differing;
    }
    return out;
  }
  const Result<model::Game> host = LauncherHost(games, source);
  if (!host) return std::unexpected(host.error());
  out.runner_ref = host->runner_ref;
  for (const model::Game& game : games.All()) {
    if (game.id == host->id || !IsLauncherGame(game, *host)) continue;
    ++out.games;
    if (game.runner_ref != out.runner_ref) ++out.differing;
  }
  return out;
}

Result<SourceRunner> SetSourceRunner(config::Config& config, store::GameStore& games,
                                     std::string_view source, const std::string& runner_ref,
                                     bool apply_to_games) {
  if (auto problem = config::Schema::Instance().Validate("launchers.runner", runner_ref)) {
    return Err("invalid_runner", *problem);
  }
  if (!runner_ref.empty() && runner_ref != "auto") {
    const runner::RunnerRegistry registry(config);
    if (auto resolved = registry.Resolve(runner_ref); !resolved)
      return std::unexpected(resolved.error());
  }

  std::function<bool(const model::Game&)> moves;
  if (HasStoreKey(source)) {
    if (auto set = config.Set(StoreKey(source), runner_ref); !set)
      return std::unexpected(set.error());
    moves = [&](const model::Game& game) { return apply_to_games && IsSourceGame(game, source); };
  } else {
    const Result<model::Game> host = LauncherHost(games, source);
    if (!host) return std::unexpected(host.error());
    moves = [host = *host](const model::Game& game) { return IsLauncherGame(game, host); };
  }

  std::vector<std::string> changed;
  for (const model::Game& game : games.All()) {
    if (!moves(game) || game.runner_ref == runner_ref) continue;
    if (games.Update(game.id, [&](model::Game& g) { g.runner_ref = runner_ref; }))
      changed.push_back(game.id);
  }
  auto out = GetSourceRunner(config, games, source);
  if (out) out->changed = std::move(changed);
  return out;
}

}  // namespace mira::library
