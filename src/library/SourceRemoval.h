#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "api/EventBus.h"
#include "config/Config.h"
#include "core/Result.h"
#include "store/GameStore.h"

// Removing a source: uninstalls its games and the source itself (signs out
// of a store, deletes a launcher's program), then turns it off. Prefixes
// are always kept, since that's where Windows games keep their saves.
// Steam and Lutris games are only dropped from Mira; those apps own them.
namespace mira::library {

struct RemovalGame {
  std::string id;
  std::string name;
  std::string deletes;  // what gets deleted, e.g. a folder or "legendary uninstall"; empty: nothing
};

struct RemovalPlan {
  std::string source;
  std::vector<RemovalGame> games;
  std::string launcher_dir;       // a launcher's program folder, deleted
  std::vector<std::string> kept;  // prefixes and save folders left alone
  bool signs_out = false;
};

Result<RemovalPlan> PlanRemoval(const config::Config& config, const store::GameStore& games,
                                std::string_view source);

struct RemovalResult {
  int removed = 0;
  std::vector<std::string> problems;  // steps that failed; the rest still ran
};

Result<RemovalResult> RemoveSource(config::Config& config, store::GameStore& games,
                                   api::EventBus& events, std::string_view source);

// Deletes `target` if it is strictly inside one of `roots` (never a root
// itself).
Result<void> DeleteInside(const std::string& target,
                          const std::vector<std::filesystem::path>& roots);

// Deletes one game's installed files the way its source does: through
// legendary, nile or butler for Epic, Amazon and itch.io, else the install
// folder when it's inside a Mira folder (a library root, a store's install
// root, or the game's own prefix). A game run from an AppImage in a shared
// folder loses just the AppImage, and a program the game only runs (see
// RunsExternalProgram) is left alone.
Result<void> DeleteGameFiles(const config::Config& config, const model::Game& game,
                             std::span<const model::Game> library);

}  // namespace mira::library
