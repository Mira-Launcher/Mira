#pragma once

#include <string>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "library/ImportSummary.h"
#include "model/Types.h"
#include "store/GameStore.h"

namespace mira::amazon {

using AmazonImportSummary = library::ImportSummary;

// Turns every game in nile's installed.json into a Mira game, launched from
// its fuel.json the way `nile launch` would.
class AmazonImporter {
public:
  AmazonImporter(config::Config& config, store::GameStore& games, api::EventBus& events);

  Result<AmazonImportSummary> Import();

private:
  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::amazon
