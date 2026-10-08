#include "library/ImportSummary.h"

#include <algorithm>

#include "api/EventBus.h"
#include "library/PrefixNaming.h"
#include "runner/RunnerRegistry.h"

namespace mira::library {

void AddTag(std::vector<std::string>& tags, const std::string& tag) {
  if (std::ranges::find(tags, tag) == tags.end()) tags.push_back(tag);
}

void RecordImported(ImportSummary& summary, api::EventBus& events, const model::Game& game, bool existed) {
  if (existed) {
    ++summary.updated;
    events.Publish("game.updated", model::ToJson(game));
  } else {
    ++summary.added;
    summary.added_games.push_back(game);
    events.Publish("game.added", model::ToJson(game));
  }
}

void ProvisionOnImport(model::Game& game, const std::optional<model::Game>& existing, const config::Config& config,
                       const store::GameStore& games, const runner::RunnerRegistry& provisioner) {
  if (NeedsProvisioning(existing)) {
    if (game.data_dir.empty()) game.data_dir = PrefixDir(config, games, game).string();
    const model::Game provisioned = provisioner.ProvisionGame(game);
    game.runner_ref = provisioned.runner_ref;
    game.data_dir = provisioned.data_dir;
    game.status = provisioned.status;
    game.last_error = provisioned.last_error;
  } else {
    game.status = model::GameStatus::Ready;
  }
}

}  // namespace mira::library
