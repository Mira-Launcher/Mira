#include "library/ImportSummary.h"

#include <algorithm>

#include "api/EventBus.h"
#include "core/Log.h"
#include "library/PrefixNaming.h"
#include "runner/RunnerRegistry.h"

namespace mira::library {

void RecordImported(ImportSummary& summary, api::EventBus& events, const model::Game& game,
                    bool existed) {
  summary.listed.insert(game.id);
  if (existed) {
    ++summary.updated;
    events.Publish("game.updated", model::ToJson(game));
  } else {
    ++summary.added;
    summary.added_games.push_back(game);
    events.Publish("game.added", model::ToJson(game));
  }
}

bool SaveImported(ImportSummary& summary, api::EventBus& events, store::GameStore& games,
                  const std::optional<model::Game>& existing, model::Game& game,
                  std::string_view what) {
  if (auto saved = games.Merge(existing, game); !saved) {
    log::Error("failed to save {} game {}: {}", what, game.id, saved.error().message);
    summary.listed.insert(game.id);  // still installed, only not saved
    return false;
  }
  RecordImported(summary, events, game, existing.has_value());
  return true;
}

void ProvisionOnImport(model::Game& game, const std::optional<model::Game>& existing,
                       const config::Config& config, const store::GameStore& games,
                       const runner::RunnerRegistry& provisioner) {
  if (NeedsProvisioning(existing)) {
    if (game.data_dir.empty()) game.data_dir = PrefixDir(config, games, game).string();
    const model::Game provisioned = provisioner.ProvisionGame(game);
    runner::TakeRunner(game, provisioned);
    game.data_dir = provisioned.data_dir;
    game.status = provisioned.status;
    game.last_error = provisioned.last_error;
  } else {
    game.status = model::GameStatus::Ready;
  }
}

}  // namespace mira::library
