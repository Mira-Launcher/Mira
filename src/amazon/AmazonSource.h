#pragma once

#include "library/ILibrarySource.h"

namespace mira::amazon {

class AmazonSource : public library::ILibrarySource {
public:
  std::string Name() const override { return "amazon"; }
  bool CanPause() const override { return true; }

  Result<std::vector<library::CatalogEntry>> Catalog(const config::Config& config,
                                                     const store::GameStore& games) const override;

  Result<void> Install(config::Config& config, store::GameStore& games, api::EventBus& events,
                      const std::string& ref) override;
  Result<void> Update(config::Config& config, store::GameStore& games, api::EventBus& events,
                     const std::string& ref) override;
};

}  // namespace mira::amazon
