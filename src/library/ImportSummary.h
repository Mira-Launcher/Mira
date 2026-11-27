#pragma once

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "model/Types.h"

namespace mira::api {
class EventBus;
}
namespace mira::config {
class Config;
}
namespace mira::runner {
class RunnerRegistry;
}
namespace mira::store {
class GameStore;
}

namespace mira::library {

// What importing a store's installed games did.
struct ImportSummary {
  int added = 0;
  int updated = 0;

  // The newly-added games; see ScanSummary::added_games in library/Scanner.h
  // for why a metadata fetch isn't triggered in the importer.
  std::vector<model::Game> added_games;

  // Every game the import listed, by id. When the store gives its whole installed list
  // (`lists_installed`), a game of its left out was uninstalled outside Mira.
  std::set<std::string> listed;
  bool lists_installed = false;
};

// Saves an imported game over `existing` and records it as RecordImported does; false once a
// failed save is logged, the game still listed as installed. `what` names it in the log ("epic").
bool SaveImported(ImportSummary& summary, api::EventBus& events, store::GameStore& games,
                  const std::optional<model::Game>& existing, model::Game& game,
                  std::string_view what);

// Counts a game as added or updated and publishes its event.
void RecordImported(ImportSummary& summary, api::EventBus& events, const model::Game& game,
                    bool existed);

// Provisions a new or failed game's prefix, else marks it Ready.
void ProvisionOnImport(model::Game& game, const std::optional<model::Game>& existing,
                       const config::Config& config, const store::GameStore& games,
                       const runner::RunnerRegistry& provisioner);

}  // namespace mira::library
