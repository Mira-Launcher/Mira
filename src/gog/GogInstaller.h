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
// EpicInstaller re: Legendary.
class GogInstaller {
 public:
  GogInstaller(config::Config& config, store::GameStore& games, api::EventBus& events);

  Result<void> Install(const std::string& id);
  Result<void> Update(const std::string& id);

 private:
  Result<void> Run(const std::string& id);

  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::gog
