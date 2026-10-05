#pragma once

#include <string>
#include <vector>

#include "../client/Types.h"

namespace mira_gui {

// How the library grid orders its tiles.
//
// A free function over the summaries, not a method on the window, so the
// ordering is a rule about games, not widgets, and worth testing without
// an event loop.
//
// `key` is one of "name", "last_played", "playtime", "status"; anything
// else falls back to "name". Stable, and every key breaks ties by name, so
// two never-played games keep a predictable order instead of shuffling.
void SortGames(std::vector<GameSummary>& games, const std::string& key, bool descending);
// The comparison SortGames sorts by, for a sort that isn't over a vector.
bool GameLess(const GameSummary& a, const GameSummary& b, const std::string& key, bool descending);

// The keys SortGames understands, in the order a picker should offer them,
// paired with the label to show.
struct SortOption {
  const char* key;
  const char* label;
};
const std::vector<SortOption>& SortOptions();

}  // namespace mira_gui
