#include "runner/RefMigration.h"

#include <algorithm>
#include <optional>

#include "config/Schema.h"
#include "core/Log.h"
#include "runner/RunnerRegistry.h"

namespace mira::runner {
int RepointRemovedBuilds(config::Config& config, store::GameStore& games,
                         std::vector<std::string>* game_ids) {
  const RunnerRegistry registry(config);
  return RepointRefs(
      config, games,
      [&](const std::string& ref) -> std::optional<std::string> {
        if (auto stand_in = registry.StandIn(ref)) return stand_in->Reference();
        return std::nullopt;
      },
      game_ids);
}

int RepointRefs(config::Config& config, store::GameStore& games, const MovedRef& moved,
                std::vector<std::string>* game_ids,
                const std::function<bool(const model::Game&)>& moves_game) {
  int changed = 0;
  std::vector<const config::Entry*> runner_keys;
  for (const config::Entry& entry : config::Schema::Instance().Entries()) {
    if (!entry.is_runner_ref) continue;
    runner_keys.push_back(&entry);
    // A setting left at its default keeps following the default.
    if (config.Get(entry.key) == entry.default_value) continue;
    if (const auto to = moved(config.GetString(entry.key))) {
      log::Info("{}: {} is now {}", entry.key, config.GetString(entry.key), *to);
      if (config.Set(entry.key, *to)) ++changed;
    }
  }

  for (const model::Game& game : games.All()) {
    if (moves_game && !moves_game(game)) continue;
    auto to = moved(game.runner_ref);
    nlohmann::json overrides = game.overrides;
    bool overrides_changed = false;
    for (const config::Entry* entry : runner_keys) {
      if (!overrides.contains(entry->key) || !overrides[entry->key].is_string()) continue;
      if (auto override_to = moved(overrides[entry->key].get<std::string>())) {
        overrides[entry->key] = *override_to;
        overrides_changed = true;
      }
    }
    if (!to && !overrides_changed) continue;
    log::Info("{}: runner {} is now {}", game.id, game.runner_ref, to.value_or(game.runner_ref));
    const auto updated = games.Update(game.id, [&](model::Game& g) {
      if (to) g.runner_ref = *to;
      if (overrides_changed) g.overrides = overrides;
    });
    if (!updated) continue;
    ++changed;
    if (game_ids != nullptr) game_ids->push_back(game.id);
  }
  return changed;
}

}  // namespace mira::runner
