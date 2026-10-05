#pragma once

#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "library/ImportSummary.h"
#include "model/Types.h"
#include "store/GameStore.h"

namespace mira::itch {

// Reads butlerd's own installed-games state (Fetch.Caves -- a "cave" is
// butler's own word for one installed copy of a game) and upserts it into
// the same GameStore as everything else, same shape as
// epic::EpicImporter. Manual (POST /v1/itch/import), not automatic:
// butler owns this data, Mira only reads a snapshot of it on request.
class ItchImporter {
public:
  ItchImporter(config::Config& config, store::GameStore& games, api::EventBus& events);

  Result<library::ImportSummary> Import();

private:
  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::itch
