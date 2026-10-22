#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Command.h"
#include "core/Result.h"
#include "library/Catalog.h"
#include "model/Types.h"
#include "store/GameStore.h"

namespace mira::library {

// What a store adds to one launch of its game, through Mira's own runner.
struct LaunchExtras {
  std::vector<std::string> args;  // after the game's own, for this launch only
  std::map<std::string, std::string> env;
  // Argument prefixes ("-AUTH_PASSWORD=") whose value is a credential, masked in the game's log.
  std::vector<std::string> redact;
  std::vector<std::filesystem::path> remove_after_exit;
  // A program the store's running games share (GOG's comet), named in SharedPrograms. Started
  // unless it already runs, stopped once no game of the store is running.
  std::optional<Command> shared;
};

// One storefront Mira can read entitlements from and, where the source
// supports it, install/update titles through. Implemented once per
// source that fits this shape (epic, steam, gog, itch) and held
// polymorphically by SourceRegistry.h's AllSources() -- not Humble,
// which has no install/update state of its own to report on.
//
// Install/Update are blocking: the caller (api::Server's POST
// /v1/library/install|update) runs them on a detached thread and reports
// progress over EventBus. Each implementation owns its own preconditions
// (tool installed, authenticated, ...) -- the caller never special-cases
// one source.
class ILibrarySource {
 public:
  virtual ~ILibrarySource() = default;

  virtual std::string Name() const = 0;

  // Titles the account owns, read through live from the source -- never
  // persisted (see CatalogEntry's own doc comment in Catalog.h).
  virtual Result<std::vector<CatalogEntry>> Catalog(const config::Config& config,
                                                    const store::GameStore& games) const = 0;

  virtual Result<void> Install(config::Config& config, store::GameStore& games,
                               api::EventBus& events, const std::string& ref) = 0;

  // Default: not supported. Steam overrides this to report so explicitly
  // (Steam updates its own games); every other source overrides for real.
  virtual Result<void> Update(config::Config& config, store::GameStore& games,
                              api::EventBus& events, const std::string& ref);

  // Whether an install stopped partway continues where it left off when run again.
  virtual bool CanPause() const { return false; }

  // Run before Mira's runner starts `game`. A failure never blocks the launch: the caller logs
  // it and starts the game without extras. Without `sign_in`, only what hands the started
  // program no credential (GOG's comet). Default: nothing to add.
  virtual Result<LaunchExtras> BeforeLaunch(const config::Config& /*config*/,
                                            const model::Game& /*game*/, bool /*sign_in*/) const {
    return LaunchExtras{};
  }

  // The name of the program in LaunchExtras::shared, for its log and systemd scope; empty when
  // the store has none.
  virtual std::string SharedProgram() const { return {}; }
};

}  // namespace mira::library
