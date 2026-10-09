#include "steam/SteamSource.h"

#include <format>

#include "core/Command.h"
#include "runner/Exec.h"
#include "steam/SteamDetector.h"
#include "steam/SteamWebApi.h"

namespace mira::steam {

Result<std::vector<library::CatalogEntry>> SteamSource::Catalog(const config::Config& config,
                                                                const store::GameStore& games) const {
  std::vector<library::CatalogEntry> entries;
  if (!config.GetBool("steam.enabled")) return entries;

  const Result<std::vector<OwnedGame>> owned = ListOwnedGames(config);
  if (!owned) return std::unexpected(owned.error());

  for (const OwnedGame& game : *owned) {
    library::CatalogEntry entry;
    entry.source = "steam";
    entry.ref = game.appid;
    entry.title = game.name.empty() ? game.appid : game.name;
    entry.play_seconds = game.play_seconds;
    library::MarkTracked(games, entry);
    entries.push_back(std::move(entry));
  }
  return entries;
}

Result<void> SteamSource::Install(config::Config& config, store::GameStore&, api::EventBus&, const std::string& ref) {
  Command command;
  command.argv = SteamCommand(config);
  command.argv.push_back(std::format("steam://install/{}", ref));
  if (auto spawned = runner::SpawnDetached(command); !spawned) return std::unexpected(spawned.error());
  return {};
}

}  // namespace mira::steam
