#include "steam/SteamWebApi.h"

#include <json.hpp>

#include "core/Json.h"
#include "runner/Exec.h"

namespace mira::steam {
namespace {
using nlohmann::json;

// Same ceiling every other outbound call here uses: this runs off a user
// request, and an unreachable endpoint must not park a thread indefinitely.
constexpr std::string_view kMaxTime = "10";

}  // namespace

Result<std::vector<OwnedGame>> ListOwnedGames(const config::Config& config) {
  const std::string key = config.GetString("steam.web_api_key");
  if (key.empty()) {
    return Err(
        "no_steam_web_api_key", "listing the games a Steam account owns needs a Steam Web API key",
        "Add a free key from steamcommunity.com/dev/apikey.", Fix::Setting("steam.web_api_key"));
  }
  const std::string steamid = config.GetString("steam.steamid64");
  if (steamid.empty()) {
    return Err("no_steamid64", "listing the games a Steam account owns needs its Steam ID",
               "Add the account's 64-bit Steam ID.", Fix::Setting("steam.steamid64"));
  }

  const Result<runner::ExecResult> result = runner::RunCurlWithSecrets(
      {"-sSL", "--max-time", std::string(kMaxTime), "--get", "--data-urlencode",
       "steamid=" + steamid, "--data", "include_appinfo=1&include_played_free_games=1&format=json",
       "https://api.steampowered.com/IPlayerService/GetOwnedGames/v1/"},
      runner::CurlConfigLine("data-urlencode", "key=" + key));
  if (!result) return std::unexpected(result.error());

  const json parsed = json::parse(result->output, nullptr, false);
  // A bad key/id comes back as an empty {"response":{}} rather than an HTTP
  // error, so "no games" and "wrong credentials" look alike from here --
  // reported as the former, since that's also what a legitimately empty
  // account looks like and there's nothing to distinguish them by.
  if (parsed.is_discarded() || !parsed.contains("response")) {
    return Err("steam_web_api_error", "Steam's Web API didn't return a usable response");
  }

  std::vector<OwnedGame> owned;
  const json& response = parsed["response"];
  if (!response.contains("games") || !response["games"].is_array()) return owned;
  for (const json& entry : response["games"]) {
    OwnedGame game;
    if (!entry.is_object() || !entry.contains("appid") || !entry["appid"].is_number_integer())
      continue;
    game.appid = std::to_string(entry["appid"].get<std::int64_t>());
    game.name = core::JsonString(entry, "name");
    // Steam reports this in minutes; model::Game::play_seconds is seconds.
    game.play_seconds = core::JsonInt(entry, "playtime_forever") * 60;
    owned.push_back(std::move(game));
  }
  return owned;
}

}  // namespace mira::steam
