#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "model/Types.h"
#include "store/GameStore.h"

namespace mira::metadata {
class FetchQueue;
}

namespace mira::library {

struct ScanSummary {
  int added = 0;    // new games detected and auto-configured
  int missing = 0;  // previously-known games whose folder is now gone
  int restored = 0; // a previously-missing game's folder reappeared

  // The games behind `added`, for callers that want to react per-game (e.g.
  // triggering a metadata fetch), deliberately not done inside Scanner
  // itself; see the comment on metadata::FetchAsync's call sites for why.
  std::vector<model::Game> added_games;
};

// Walks every enabled library root one level deep: each immediate
// subdirectory is treated as one game, matching "drop a game folder in and
// it's picked up". Recursing further is Detector's job, scoped to inside a
// single already-identified game folder.
//
// A directory already known to GameStore (by install_path) is never
// re-detected, so a scan never overwrites a user's changes. Scan only adds
// new games, reconciles missing/restored ones and retries provisioning.
class Scanner {
public:
  Scanner(config::Config& config, store::GameStore& games, api::EventBus& events);

  ScanSummary ScanAll();
  ScanSummary ScanRoot(const std::filesystem::path& root);

  // Where an installer it runs on its own (scan.auto_run_installers) queues the installed
  // game's art. Must outlive the install, which continues after the scan.
  void UseMetadataQueue(metadata::FetchQueue& queue) { metadata_fetches_ = &queue; }

private:
  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
  metadata::FetchQueue* metadata_fetches_ = nullptr;
};

// Provisions again every Windows game left broken by a missing or failing
// runner, once a runner resolves for it. Returns how many became ready.
int RetryBrokenProvisioning(config::Config& config, store::GameStore& games, api::EventBus& events);
// Same for one game. Returns whether it became ready.
bool RetryBrokenProvisioning(config::Config& config, store::GameStore& games, api::EventBus& events,
                             const std::string& id);

}  // namespace mira::library
