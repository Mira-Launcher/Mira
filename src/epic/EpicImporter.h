#pragma once

#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "library/ImportSummary.h"
#include "model/Types.h"
#include "store/GameStore.h"

namespace mira::epic {

using EpicImportSummary = library::ImportSummary;

// Reads Legendary's own installed-titles list (and, if epic.import_uninstalled
// is on, its full catalog too) and upserts them into the same GameStore as
// everything else: an Epic game is a normal model::Game, runner_ref left
// empty so RunnerRegistry resolves a real Wine/Proton build for it, exactly
// like a Lutris "wine" row. Manual (POST /v1/epic/import), not
// inotify-driven, same reasoning as Steam/Lutris: Legendary owns this data,
// Mira only reads a snapshot of it on request.
//
// Unlike Steam/Lutris, an installed-but-not-yet-provisioned Epic game still
// needs RunnerRegistry::ProvisionGame run on it once (see EpicInstaller),
// Legendary has no prefix concept of its own, so a freshly-imported
// already-installed title is upserted Ready with an empty data_dir, and
// gets provisioned lazily the same way a plain scan-detected game does.
class EpicImporter {
public:
  EpicImporter(config::Config& config, store::GameStore& games, api::EventBus& events);

  Result<EpicImportSummary> Import();

private:
  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::epic
