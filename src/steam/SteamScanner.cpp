#include "steam/SteamScanner.h"

#include <algorithm>
#include <map>

#include "core/Log.h"
#include "steam/SteamDetector.h"
#include "steam/SteamWebApi.h"

namespace mira::steam {
namespace {

// appid -> Steam's own playtime_forever, in seconds. Empty when the Web API
// isn't configured (the normal case) or unreachable. Playtime import is
// strictly an enrichment on top of a scan that works fine without it.
std::map<std::string, std::int64_t> PlaytimeByAppid(const config::Config& config) {
  std::map<std::string, std::int64_t> playtime;
  if (!config.GetBool("steam.import_playtime") || config.GetString("steam.web_api_key").empty()) return playtime;

  const Result<std::vector<OwnedGame>> owned = ListOwnedGames(config);
  if (!owned) {
    log::Warn("steam playtime not imported: {}", owned.error().message);
    return playtime;
  }
  for (const OwnedGame& game : *owned) playtime[game.appid] = game.play_seconds;
  return playtime;
}

}  // namespace

SteamScanner::SteamScanner(config::Config& config, store::GameStore& games, api::EventBus& events)
    : config_(config), games_(games), events_(events) {}

Result<library::ImportSummary> SteamScanner::Scan() {
  library::ImportSummary summary;
  if (!games_.SourceEnabled("steam")) return summary;

  const auto root = FindSteamRoot(config_);
  if (!root) {
    return Err("steam_not_found", "Mira couldn't find a Steam installation",
               "If Steam is installed somewhere unusual, set where it is.", Fix::Setting("steam.root"));
  }

  const std::map<std::string, std::int64_t> steam_playtime = PlaytimeByAppid(config_);
  const std::map<std::string, AppActivity> activity = config_.GetBool("steam.import_playtime")
                                                          ? ReadAppActivity(*root, config_.GetString("steam.steamid64"))
                                                          : std::map<std::string, AppActivity>{};

  for (const SteamApp& app : ListApps(*root)) {
    const std::string id = "steam-" + app.appid;
    const auto existing = games_.Find(id);

    // Preserve anything the user already configured across a rescan
    // (exe_path/args/env for "direct" mode, overrides, reviewed), only the
    // fields Steam itself owns get overwritten.
    model::Game game = existing.value_or(model::Game{});
    game.id = id;
    game.source = "steam";
    game.name = app.name;
    game.install_path = app.install_dir.string();
    game.platform = app.is_native ? model::Platform::Native : model::Platform::Windows;
    game.data_dir = app.compat_data_dir.string();
    game.runner_ref = "steam:" + app.appid;
    game.status = model::GameStatus::Ready;  // Steam already installed and provisioned it
    game.last_error.clear();
    // Steam's own total counts play on any machine, and from long before
    // Mira existed -- but Mira's own tracked sessions must never be lost to
    // a stale Steam figure, so the larger of the two wins rather than
    // Steam's simply overwriting.
    if (const auto it = steam_playtime.find(app.appid); it != steam_playtime.end()) {
      game.play_seconds = std::max(game.play_seconds, it->second);
    }
    // Steam's own record covers launches from Steam itself; the later date and larger total win.
    if (const auto it = activity.find(app.appid); it != activity.end()) {
      game.play_seconds = std::max(game.play_seconds, it->second.play_seconds);
      if (it->second.last_played_at > game.last_played_at.value_or(0)) {
        game.last_played_at = it->second.last_played_at;
      }
    }
    game.updated_at = model::NowSeconds();
    if (!existing) game.created_at = game.updated_at;

    auto result = games_.Merge(existing, game);
    if (!result) {
      log::Error("failed to save steam game {}: {}", id, result.error().message);
      continue;
    }
    library::RecordImported(summary, events_, game, existing.has_value());
  }
  return summary;
}

}  // namespace mira::steam
