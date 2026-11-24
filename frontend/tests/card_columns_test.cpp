#include <algorithm>

#include "doctest.h"
#include "game/CardColumns.h"

using namespace mira_gui;

namespace {

GameDetail Game(const std::string& source, const std::string& platform,
                const std::string& runner_ref = {}) {
  GameDetail game;
  game.source = source;
  game.platform = platform;
  game.runner_ref = runner_ref;
  return game;
}

bool Has(const std::vector<QString>& cards, const char* title) {
  return std::ranges::contains(cards, QString(title));
}

bool Anywhere(const CardColumns& columns, const char* title) {
  return Has(columns.runs, title) || Has(columns.game, title);
}

}  // namespace

TEST_CASE("each kind of game gets the cards for how it runs") {
  const CardColumns steam = CardColumnsFor(Game("steam", "windows"));
  CHECK(Has(steam.runs, "Steam"));
  CHECK_FALSE(Anywhere(steam, "Runner"));
  CHECK_FALSE(Anywhere(steam, "Launch"));
  CHECK_FALSE(Anywhere(steam, "Emulator"));

  const CardColumns emulated = CardColumnsFor(Game("scan", "gba", "emulator:mgba"));
  CHECK(Has(emulated.runs, "Emulator"));
  CHECK_FALSE(Anywhere(emulated, "Runner"));
  CHECK_FALSE(Anywhere(emulated, "Launch"));

  const CardColumns epic = CardColumnsFor(Game("epic", "windows", "proton"), "Epic Games");
  CHECK(Has(epic.runs, "Runner"));
  CHECK(Has(epic.runs, "Epic Games"));
  CHECK(Has(epic.game, "Launch"));
  CHECK_FALSE(Anywhere(epic, "Steam"));
  // The store's card follows the runner it goes with.
  const auto runner = std::ranges::find(epic.runs, QString("Runner"));
  const auto store = std::ranges::find(epic.runs, QString("Epic Games"));
  CHECK(runner < store);

  // Every game has its details, saves and files, on the game's side.
  for (const CardColumns& columns : {steam, emulated, epic}) {
    CHECK(Has(columns.game, "Details"));
    CHECK(Has(columns.game, "Saves"));
    CHECK(Has(columns.game, "Files"));
    CHECK(columns.game.front() == "Details");
  }
}

TEST_CASE("only a game whose prefix Mira looks after shows the prefix row") {
  CHECK(ShowsPrefixRow(Game("scan", "windows", "proton")));
  CHECK_FALSE(ShowsPrefixRow(Game("scan", "web")));  // its data folder is its browser profile
  CHECK_FALSE(ShowsPrefixRow(Game("scan", "native")));
  CHECK_FALSE(ShowsPrefixRow(Game("desktop-entry", "windows")));
  CHECK_FALSE(ShowsPrefixRow(Game("scan", "windows", "emulator:dolphin")));

  GameDetail steam = Game("steam", "windows");
  CHECK_FALSE(ShowsPrefixRow(steam));
  steam.data_dir = "/games/steam/compatdata/413150";
  CHECK(ShowsPrefixRow(steam));
}
