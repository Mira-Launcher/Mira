#pragma once

#include "library/ILibrarySource.h"

namespace mira::itch {

// Catalog via butlerd's Fetch.ProfileOwnedKeys; Install/Update wrap
// ItchInstaller.
class ItchSource : public library::ILibrarySource {
 public:
  std::string Name() const override { return "itch"; }

  Result<std::vector<library::CatalogEntry>> Catalog(const config::Config& config,
                                                     const store::GameStore& games) const override;

  Result<void> Install(config::Config& config, store::GameStore& games, api::EventBus& events,
                       const std::string& ref) override;
  Result<void> Update(config::Config& config, store::GameStore& games, api::EventBus& events,
                      const std::string& ref) override;
};

}  // namespace mira::itch
