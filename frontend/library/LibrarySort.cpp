#include "LibrarySort.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace mira_gui {
namespace {

// Case-insensitive, so "blue prince" sorts next to "Blue Prince" rather
// than after every capitalised title: game names come from folder names
// and their capitalisation is not meaningful.
int CompareNames(const std::string& left, const std::string& right) {
  const size_t shared = std::min(left.size(), right.size());
  for (size_t i = 0; i < shared; ++i) {
    const unsigned char l = static_cast<unsigned char>(std::tolower(left[i]));
    const unsigned char r = static_cast<unsigned char>(std::tolower(right[i]));
    if (l != r) return l < r ? -1 : 1;
  }
  if (left.size() == right.size()) return 0;
  return left.size() < right.size() ? -1 : 1;
}

// A game that has never been played sorts as older than any that has, in
// both directions: "never" is not a date, and treating it as 0 would put
// it next to 1970 rather than at the end.
std::int64_t LastPlayedRank(const GameSummary& game) {
  return game.last_played_at.value_or(std::numeric_limits<std::int64_t>::min());
}

}  // namespace

const std::vector<SortOption>& SortOptions() {
  static const std::vector<SortOption> options = {
      {"name", "Name"},
      {"last_played", "Last played"},
      {"playtime", "Playtime"},
      {"status", "Status"},
  };
  return options;
}

bool GameLess(const GameSummary& a, const GameSummary& b, const std::string& key, bool descending) {
  // `descending` inverts the primary key only; the name tiebreak always
  // reads A to Z. Reversing the whole result instead would flip the
  // tiebreak too, so games sharing a timestamp would come out Z to A.
  const auto ordered = [descending](auto left, auto right) { return descending ? right < left : left < right; };
  if (key == "last_played") {
    const std::int64_t ra = LastPlayedRank(a);
    const std::int64_t rb = LastPlayedRank(b);
    if (ra != rb) return ordered(ra, rb);
  } else if (key == "playtime") {
    if (a.play_seconds != b.play_seconds) return ordered(a.play_seconds, b.play_seconds);
  } else if (key == "status") {
    if (a.status != b.status) return ordered(a.status, b.status);
  } else {
    const int by_name = CompareNames(a.name, b.name);
    if (by_name != 0) return ordered(by_name, 0);
  }
  return CompareNames(a.name, b.name) < 0;
}

void SortGames(std::vector<GameSummary>& games, const std::string& key, bool descending) {
  std::stable_sort(games.begin(), games.end(), [&](const GameSummary& a, const GameSummary& b) {
    return GameLess(a, b, key, descending);
  });
}

}  // namespace mira_gui
