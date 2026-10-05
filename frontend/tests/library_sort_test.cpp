#include <doctest.h>

#include <vector>

#include "ui/LibrarySort.h"

using namespace mira_gui;

namespace {

GameSummary Game(const char* name, std::optional<std::int64_t> last_played,
                 std::int64_t play_seconds = 0, const char* status = "ready") {
  GameSummary game;
  game.id = name;
  game.name = name;
  game.status = status;
  game.last_played_at = last_played;
  game.play_seconds = play_seconds;
  return game;
}

std::vector<std::string> Names(const std::vector<GameSummary>& games) {
  std::vector<std::string> names;
  for (const GameSummary& game : games) names.push_back(game.name);
  return names;
}

}  // namespace

TEST_CASE("SortGames orders by name case-insensitively") {
  // Game names come from folder names, so capitalisation is an accident,
  // a plain byte compare would file every lowercase title after every
  // uppercase one.
  std::vector<GameSummary> games = {Game("wandering sword", 1), Game("Animal Well", 2),
                                    Game("blue prince", 3)};
  SortGames(games, "name", false);
  CHECK(Names(games) == std::vector<std::string>{"Animal Well", "blue prince", "wandering sword"});
}

TEST_CASE("SortGames sorts never-played games as older than any played one") {
  // "Never" is not a date. Treating it as 0 would file it next to 1970,
  // which is a real timestamp, so it belongs at the far end instead.
  std::vector<GameSummary> games = {Game("played", 1789620825), Game("never", std::nullopt),
                                    Game("epoch", 0)};

  SortGames(games, "last_played", false);
  CHECK(Names(games) == std::vector<std::string>{"never", "epoch", "played"});

  SortGames(games, "last_played", true);
  CHECK(Names(games) == std::vector<std::string>{"played", "epoch", "never"});
}

TEST_CASE("Descending inverts the key but never the name tiebreak") {
  // Games sharing a value still read A to Z, both ways round. Reversing the
  // finished sort instead would flip them to Z to A.
  std::vector<GameSummary> games = {Game("Charlie", std::nullopt), Game("alpha", std::nullopt),
                                    Game("Bravo", std::nullopt)};

  SortGames(games, "last_played", true);
  CHECK(Names(games) == std::vector<std::string>{"alpha", "Bravo", "Charlie"});

  SortGames(games, "playtime", true);
  CHECK(Names(games) == std::vector<std::string>{"alpha", "Bravo", "Charlie"});
}

TEST_CASE("SortGames orders by playtime and by status") {
  std::vector<GameSummary> games = {Game("a", std::nullopt, 60), Game("b", std::nullopt, 7200),
                                    Game("c", std::nullopt, 0)};
  SortGames(games, "playtime", true);
  CHECK(Names(games) == std::vector<std::string>{"b", "a", "c"});

  std::vector<GameSummary> by_status = {Game("x", std::nullopt, 0, "ready"),
                                        Game("y", std::nullopt, 0, "broken"),
                                        Game("z", std::nullopt, 0, "needs_install")};
  SortGames(by_status, "status", false);
  CHECK(Names(by_status) == std::vector<std::string>{"y", "z", "x"});
}

TEST_CASE("An unknown sort key falls back to name instead of leaving it unordered") {
  // frontend.toml is hand-editable and may come from a newer build, so an
  // unrecognised key must still produce a deterministic grid.
  std::vector<GameSummary> games = {Game("b", std::nullopt), Game("a", std::nullopt)};
  SortGames(games, "nonsense", false);
  CHECK(Names(games) == std::vector<std::string>{"a", "b"});
}
