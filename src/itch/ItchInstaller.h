#pragma once

#include <string>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "store/GameStore.h"

namespace mira::itch {

// Installs/updates one itch.io title via butlerd's Install.Queue ->
// Install.Perform sequence, then re-runs ItchImporter to pick up the resulting cave as a tracked
// model::Game and provision a Wine/Proton prefix for it if the upload
// isn't a native Linux build.
Result<void> InstallTitle(config::Config& config, store::GameStore& games, api::EventBus& events,
                          const std::string& game_id);

}  // namespace mira::itch
