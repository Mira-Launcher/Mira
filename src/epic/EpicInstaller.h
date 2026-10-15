#pragma once

#include <string>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "store/GameStore.h"

namespace mira::epic {

// Installs or updates one Epic title via `legendary install`/`update`.
// Blocking: the caller (api::Server's POST /v1/epic/install|update
// routes) runs this on a detached thread and reports progress over
// EventBus, the same "don't block the request thread on a slow download"
// pattern used for Proton-GE (see runner::DownloadAndInstall's caller). On
// success, re-runs EpicImporter, which is what actually picks up the new
// install and provisions a Wine/Proton prefix for it. Legendary itself
// never creates one (see EpicImporter.h's class comment).
class EpicInstaller {
 public:
  EpicInstaller(config::Config& config, store::GameStore& games, api::EventBus& events);

  Result<void> Install(const std::string& app_name);
  Result<void> Update(const std::string& app_name);

 private:
  Result<void> Run(const std::string& verb, const std::string& app_name);

  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::epic
