#pragma once

#include "library/ILibrarySource.h"

namespace mira::steam {

// Wraps SteamWebApi's owned-games listing for the catalog side, and hands
// install off to the Steam client itself (steam://install/<appid>) --
// same behavior as before this existed, just reachable through
// library::AllSources() instead of a hand-written "if source == steam"
// branch.
class SteamSource : public library::ILibrarySource {
 public:
  std::string Name() const override { return "steam"; }

  Result<std::vector<library::CatalogEntry>> Catalog(const config::Config& config,
                                                     const store::GameStore& games) const override;

  // Fires steam://install/<ref> and returns -- Steam's own client owns the
  // download from here, same posture as steam.launch_mode "steam".
  Result<void> Install(config::Config& config, store::GameStore& games, api::EventBus& events,
                       const std::string& ref) override;

  // Steam updates its own games; nothing for Mira to do. Falls through to
  // ILibrarySource's default "unsupported" body.
};

}  // namespace mira::steam
