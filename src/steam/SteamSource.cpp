#include "steam/SteamSource.h"

#include <format>

#include "core/Command.h"
#include "runner/Exec.h"
#include "steam/AppCache.h"
#include "steam/SteamDetector.h"
#include "steam/SteamWebApi.h"

namespace mira::steam {

Result<std::vector<library::CatalogEntry>> SteamSource::Catalog(
    const config::Config& config, const store::GameStore& games) const {
  std::vector<library::CatalogEntry> entries;
  if (!games.SourceEnabled("steam")) return entries;

  std::vector<OwnedGame> owned;
  if (config.GetString("steam.web_api_key").empty()) {
    const auto root = FindSteamRoot(config);
    if (!root) return entries;
    owned = OwnedFromAppCache(*root);
    const auto activity = ReadAppActivity(*root, config.GetString("steam.steamid64"));
    for (OwnedGame& game : owned) {
      if (const auto found = activity.find(game.appid); found != activity.end()) {
        game.play_seconds = found->second.play_seconds;
      }
    }
  } else {
    Result<std::vector<OwnedGame>> listed = ListOwnedGames(config);
    if (!listed) return std::unexpected(listed.error());
    owned = std::move(*listed);
  }

  for (const OwnedGame& game : owned) {
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

Result<void> SteamSource::Install(config::Config& config, store::GameStore&, api::EventBus&,
                                  const std::string& ref) {
  Command command;
  command.argv = SteamCommand(config);
  command.argv.push_back(std::format("steam://install/{}", ref));
  if (auto spawned = runner::SpawnReaped(command); !spawned)
    return std::unexpected(spawned.error());
  return {};
}

}  // namespace mira::steam
