#pragma once

#include "library/ILibrarySource.h"

namespace mira::gog {

// Catalog reads GOG's own embed.gog.com API directly (bearer token from
// Gog.h's AccessToken) rather than through gogdl -- gogdl has no
// list-owned-games subcommand of its own (see Gog.h's class comment).
// Install/Update wrap GogInstaller.
class GogSource : public library::ILibrarySource {
public:
  std::string Name() const override { return "gog"; }
  bool CanPause() const override { return true; }

  Result<std::vector<library::CatalogEntry>> Catalog(const config::Config& config,
                                                     const store::GameStore& games) const override;

  Result<void> Install(config::Config& config, store::GameStore& games, api::EventBus& events,
                      const std::string& ref) override;
  Result<void> Update(config::Config& config, store::GameStore& games, api::EventBus& events,
                     const std::string& ref) override;
};

}  // namespace mira::gog
