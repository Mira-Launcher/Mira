#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "config/Config.h"
#include "core/Result.h"
#include "model/Types.h"

namespace mira::desktop {

// Keeps the application menu in sync with the library, so a game can be
// started from the desktop like any other app.
//
// Entries always launch *through* Mira (`mira launch <id>`, or the frontend)
// rather than running the game's executable directly: a direct Exec= would
// start the game fine and record nothing, so playtime and game.state events
// would silently be wrong for every menu launch.
//
// Only ever touches files it created (`mira-<id>.desktop`), never anything
// else in the directory.
class DesktopEntries {
public:
  explicit DesktopEntries(config::Config& config);

  // Writes an entry per launchable game and removes the ones that no longer
  // apply (deleted, missing, broken, or still needing an install). A no-op
  // when desktop_entries.enabled is false, except that it then removes any
  // entries previously written.
  Result<void> Sync(const std::vector<model::Game>& games);

  // Writes or removes just the entry of `game_id` (`game` is empty once it's deleted), for a change to
  // one game, where Sync's pass over every game and the whole folder would be wasted.
  Result<void> SyncOne(const std::string& game_id, const std::optional<model::Game>& game);

private:
  std::filesystem::path EntryPath(const std::string& game_id) const;
  std::string Render(const model::Game& game) const;
  bool IsWanted(const model::Game& game) const;

  config::Config& config_;
};

}  // namespace mira::desktop
