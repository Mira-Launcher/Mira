#pragma once

#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "library/ImportSummary.h"
#include "model/Types.h"
#include "store/GameStore.h"

namespace mira::steam {

// Detects installed Steam games and upserts them into the same GameStore
// as everything else: a Steam game is a normal model::Game with
// runner_ref "steam:<appid>", so every existing endpoint (GET /v1/games,
// PATCH, launch, stop) already works on it without modification. Run by
// POST /v1/steam/scan, and by the library Watcher once Steam stops writing an
// app manifest (an install or uninstall finishing in Steam).
class SteamScanner {
 public:
  SteamScanner(config::Config& config, store::GameStore& games, api::EventBus& events);

  Result<library::ImportSummary> Scan();

 private:
  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::steam
