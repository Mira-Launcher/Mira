#include "itch/ItchSource.h"

#include <unordered_map>
#include <unordered_set>

#include <json.hpp>

#include "itch/Butlerd.h"
#include "itch/Itch.h"
#include "itch/ItchInstaller.h"

namespace mira::itch {
namespace {
using nlohmann::json;

// Loops a paginated Fetch.* call (cursor -> nextCursor) until it stops
// returning one, accumulating every "items" array along the way -- some
// bundles run into the hundreds of games, so a single page isn't enough.
Result<std::vector<json>> FetchAllPages(const config::Config& config, const std::string& method, json params) {
  std::vector<json> items;
  while (true) {
    const Result<json> page = Call(config, method, params);
    if (!page) return std::unexpected(page.error());
    for (const json& item : page->value("items", json::array())) items.push_back(item);
    if (!page->contains("nextCursor") || (*page)["nextCursor"].is_null()) break;
    params["cursor"] = (*page)["nextCursor"];
  }
  return items;
}

}  // namespace

Result<std::vector<library::CatalogEntry>> ItchSource::Catalog(const config::Config& config,
                                                               const store::GameStore& games) const {
  std::vector<library::CatalogEntry> entries;
  if (!games.SourceEnabled("itch")) return entries;

  const Result<std::int64_t> profile_id = CurrentProfileId(config);
  if (!profile_id) return std::unexpected(profile_id.error());

  std::unordered_set<std::int64_t> seen;
  const auto add_entry = [&](std::int64_t game_id, const std::string& title, bool owned) {
    if (game_id == 0 || seen.contains(game_id)) return;
    seen.insert(game_id);
    library::CatalogEntry entry;
    entry.source = "itch";
    entry.ref = std::to_string(game_id);
    entry.title = title.empty() ? entry.ref : title;
    entry.owned = owned;
    library::MarkTracked(games, entry);
    entries.push_back(std::move(entry));
  };

  // Individually purchased/redeemed titles.
  const Result<std::vector<json>> keys =
    FetchAllPages(config, "Fetch.ProfileOwnedKeys", {{"profileId", *profile_id}, {"fresh", true}});
  if (!keys) return std::unexpected(keys.error());
  for (const json& key : *keys) {
    // DownloadKey.game is optional per butlerd's own spec -- gameId is the
    // one field that's always there.
    const json& game = key.value("game", json::object());
    add_entry(key.value("gameId", game.value("id", std::int64_t{0})), game.value("title", std::string()), true);
  }

  // Bundle ownership is a separate model entirely (confirmed against
  // butlerd's own spec) -- a game bought only as part of a bundle never
  // shows up as a DownloadKey until something "materializes" it (e.g. an
  // install), so the bundles themselves have to be walked too.
  const Result<std::vector<json>> bundle_keys =
    FetchAllPages(config, "Fetch.ProfileOwnedBundles", {{"profileId", *profile_id}, {"fresh", true}});
  if (!bundle_keys) return std::unexpected(bundle_keys.error());

  for (const json& bundle_key : *bundle_keys) {
    const std::int64_t bundle_id = bundle_key.value("bundleId", std::int64_t{0});
    if (bundle_id == 0) continue;
    const Result<std::vector<json>> bundle_games =
      FetchAllPages(config, "Fetch.BundleGames",
                   {{"profileId", *profile_id}, {"bundleId", bundle_id}, {"fresh", true}});
    if (!bundle_games) continue;  // one broken bundle shouldn't hide the rest
    for (const json& bundle_game : *bundle_games) {
      const json& game = bundle_game.value("game", json::object());
      add_entry(bundle_game.value("gameId", game.value("id", std::int64_t{0})), game.value("title", std::string()),
                true);
    }
  }

  // Your own collections and the ones added by link. A free game (butlerd
  // omits minPrice for those) can be downloaded without a key; a paid one is
  // listed but not owned. Best effort.
  if (const Result<std::vector<ItchCollection>> collections = ListCollections(config)) {
    for (const ItchCollection& collection : *collections) {
      const Result<std::vector<json>> collection_games =
        FetchAllPages(config, "Fetch.Collection.Games",
                      {{"profileId", *profile_id}, {"collectionId", collection.id}, {"fresh", true}});
      if (!collection_games) continue;
      for (const json& collection_game : *collection_games) {
        const json& game = collection_game.value("game", json::object());
        add_entry(collection_game.value("gameId", game.value("id", std::int64_t{0})),
                  game.value("title", std::string()), game.value("minPrice", std::int64_t{0}) == 0);
      }
    }
  }

  return entries;
}

Result<void> ItchSource::Install(config::Config& config, store::GameStore& games, api::EventBus& events,
                                const std::string& ref) {
  ItchInstaller installer(config, games, events);
  return installer.Install(ref);
}

Result<void> ItchSource::Update(config::Config& config, store::GameStore& games, api::EventBus& events,
                               const std::string& ref) {
  ItchInstaller installer(config, games, events);
  return installer.Update(ref);
}

}  // namespace mira::itch
