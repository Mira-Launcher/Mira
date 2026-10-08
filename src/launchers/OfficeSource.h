#pragma once

#include "library/ILibrarySource.h"

namespace mira::launchers {

// Microsoft 365's apps as a library: once Microsoft 365 is set up, each app installs and uninstalls on its own.
class OfficeSource : public library::ILibrarySource {
public:
  std::string Name() const override { return "office"; }

  Result<std::vector<library::CatalogEntry>> Catalog(const config::Config& config,
                                                     const store::GameStore& games) const override;

  Result<void> Install(config::Config& config, store::GameStore& games, api::EventBus& events,
                      const std::string& ref) override;
};

}  // namespace mira::launchers
