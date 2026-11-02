#pragma once

#include <filesystem>
#include <optional>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "library/ImportSummary.h"
#include "store/GameStore.h"

// Heroic Games Launcher's installed games: Epic (through its legendary), GOG, Amazon (nile) and
// sideloaded ones, read from Heroic's own files. They run through Mira's runners in Heroic's
// prefixes, as Lutris's games do.
namespace mira::heroic {

// Heroic's config folder: heroic.config_dir, else $XDG_CONFIG_HOME/heroic, else the Flatpak's.
std::optional<std::filesystem::path> FindHeroicConfigDir(const config::Config& config);

struct HeroicImportSummary : library::ImportSummary {
  int unsupported = 0;  // a macOS or browser game
  int incomplete = 0;   // no program or prefix to be found
};

class HeroicImporter {
 public:
  HeroicImporter(config::Config& config, store::GameStore& games, api::EventBus& events);

  Result<HeroicImportSummary> Import();

 private:
  config::Config& config_;
  store::GameStore& games_;
  api::EventBus& events_;
};

}  // namespace mira::heroic
