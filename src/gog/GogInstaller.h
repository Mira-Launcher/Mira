#pragma once

#include <string>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "store/GameStore.h"

namespace mira::gog {

// Downloads/updates one GOG title via `gogdl download`, into
// "<gog.install_root>/<id>", then runs GogImporter::ImportPath on that
// directory to pick it up as a tracked model::Game and provision a
// Wine/Proton prefix for it. gogdl never creates one, the same posture as
// epic::InstallTitle re: Legendary.
Result<void> InstallTitle(config::Config& config, store::GameStore& games, api::EventBus& events,
                          const std::string& id);

}  // namespace mira::gog
