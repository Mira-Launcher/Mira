#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "library/FolderTags.h"
#include "library/UnclearMoves.h"
#include "model/Types.h"
#include "store/GameStore.h"

namespace mira {
class Lane;
}
namespace mira::metadata {
class FetchQueue;
}

namespace mira::library {

struct ScanSummary {
  int added = 0;    // new games detected and auto-configured
  int missing = 0;  // previously-known games whose folder is now gone
  int restored = 0; // a previously-missing game's folder reappeared
  int moved = 0;    // a known game's folder was moved by hand and followed

  // The games behind `added`, for callers that want to react per-game (e.g.
  // triggering a metadata fetch), deliberately not done inside Scanner
  // itself; see the comment on metadata::FetchAsync's call sites for why.
  std::vector<model::Game> added_games;
  // Folders that could be any of several games moved by hand; neither added nor marked missing.
  std::vector<UnclearMove> unclear;
};

// Walks every enabled library root one level deep: each immediate
// subdirectory is treated as one game, matching "drop a game folder in and
// it's picked up", except the folders tag sorting uses (.hidden, a folder
// tag's folder; see FolderTags.h), whose subdirectories are games instead.
// Recursing further is Detector's job, scoped to inside a single
// already-identified game folder. A known game whose folder was moved by hand
// within the library roots is followed, not marked missing.
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
  // Where an installer it runs on its own is run; without one, scans skip those installs.
  void UseInstallLane(Lane& lane) { installs_ = &lane; }
  // Where the unclear moves it finds are kept for clients to settle.
  void UseUnclearMoves(UnclearMoves& moves) { unclear_moves_ = &moves; }

  // Settles an unclear move: `folder` is game `id`'s, moved by hand, or a new game without one.
  Result<model::Game> Settle(const std::filesystem::path& folder,
                             const std::optional<std::string>& id);

private:
  // Points `moved` at `found`, with its tags following the new place under `root`.
  Result<model::Game> Follow(const model::Game& moved, const std::filesystem::path& found,
                             bool loose, const std::filesystem::path& root, const SortRules& rules);

  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
  metadata::FetchQueue* metadata_fetches_ = nullptr;
  Lane* installs_ = nullptr;
  UnclearMoves* unclear_moves_ = nullptr;
};

// Provisions again every Windows game left broken by a missing or failing
// runner, once a runner resolves for it. Returns how many became ready.
int RetryBrokenProvisioning(config::Config& config, store::GameStore& games, api::EventBus& events);
// Same for one game. Returns whether it became ready.
bool RetryBrokenProvisioning(config::Config& config, store::GameStore& games, api::EventBus& events,
                             const std::string& id);

}  // namespace mira::library
