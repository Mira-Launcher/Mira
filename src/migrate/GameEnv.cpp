#include "migrate/GameEnv.h"

#include <string>
#include <vector>

#include "core/Log.h"
#include "launchers/Launchers.h"

namespace mira::migrate {
namespace {
using nlohmann::json;

// Variables the game's store or launcher set on it, which stay in its `env`.
bool StoreOwned(const model::Game& game, const std::string& key) {
  if (game.source == "amazon") return key.starts_with("FUEL_") || key.starts_with("AMAZON_GAMES_");
  const launchers::Launcher* launcher = launchers::ForGame(game);
  return launcher != nullptr && launcher->env.contains(key);
}

}  // namespace

void MoveGameEnv(const config::Config& config, store::GameStore& games) {
  int moved = 0;
  for (const model::Game& game : games.All()) {
    std::vector<std::string> keys;
    for (const auto& [key, value] : game.env)
      if (!StoreOwned(game, key)) keys.push_back(key);
    if (keys.empty()) continue;
    auto updated = games.Update(game.id, [&](model::Game& g) {
      json list = g.overrides.contains("launch.env") && g.overrides["launch.env"].is_array()
                      ? g.overrides["launch.env"]
                      : config.Get("launch.env");
      if (!list.is_array()) list = json::array();
      for (const std::string& key : keys) {
        config::SetEnvEntry(list, key, g.env[key]);
        g.env.erase(key);
      }
      g.overrides["launch.env"] = std::move(list);
    });
    if (updated) ++moved;
  }
  if (moved > 0) log::Info("moved {} game(s)' environment variables into launch.env", moved);
}

}  // namespace mira::migrate
