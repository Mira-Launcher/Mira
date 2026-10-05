#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "library/ImportSummary.h"
#include "model/Types.h"
#include "store/GameStore.h"

namespace mira::gog {

// The installed game directory under gog.install_root holding
// goggame-<id>.info, or empty if none does.
std::filesystem::path FindGameDir(const config::Config& config, const std::string& id);

// Unlike epic::EpicImporter, gogdl has no "list what's installed" of its
// own (see Gog.h), so the only source of truth is gog.install_root, the
// directory Mira itself installs into. Import() walks its immediate
// subdirectories and runs `gogdl import <dir>` on each to identify it, so
// a game GogInstaller already put there is picked up on a fresh `mirad`
// start, and one a user drops in by hand is picked up too.
class GogImporter {
public:
  GogImporter(config::Config& config, store::GameStore& games, api::EventBus& events);

  Result<library::ImportSummary> Import();

  // Identifies and upserts one already-unpacked install at `path` --
  // what GogInstaller calls right after a fresh download (already knows
  // `id`, GOG's product id), and what `mira gog import <id> <path>` uses
  // for an install living somewhere else. A missing/unparseable title
  // falls back to `id`, same as every other source here. `refresh` false
  // skips gogdl for a game already imported from this folder with an exe.
  Result<model::Game> ImportPath(const std::string& id, const std::filesystem::path& path, bool refresh = true);

private:
  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::gog
