#include <doctest.h>

#include <QAbstractItemModelTester>
#include <QPersistentModelIndex>

#include <string>
#include <vector>

#include "library/GameLibraryModel.h"
#include "library/GameTileDelegate.h"

using namespace mira_gui;

namespace {

GameSummary Game(const std::string& id, const std::string& name, std::vector<std::string> tags = {},
                 const std::string& source = "scan") {
  GameSummary game;
  game.id = id;
  game.name = name;
  game.status = "ready";
  game.tags = std::move(tags);
  game.source = source;
  return game;
}

std::vector<std::string> Shown(const GameFilterProxy& proxy) {
  std::vector<std::string> ids;
  for (int row = 0; row < proxy.rowCount(); ++row) {
    ids.push_back(proxy.index(row, 0).data(GameTileDelegate::IdRole).toString().toStdString());
  }
  return ids;
}

}  // namespace

TEST_CASE("A relist keeps the rows it still lists, so a view's selection survives it") {
  GameLibraryModel library;
  library.Replace({Game("a", "Alpha"), Game("b", "Beta"), Game("c", "Gamma")});
  const QPersistentModelIndex beta = library.IndexOf("b");

  GameSummary renamed = Game("b", "Beta Remastered");
  library.Replace({Game("a", "Alpha"), renamed});

  REQUIRE(beta.isValid());
  CHECK(beta.data(GameTileDelegate::NameRole).toString() == "Beta Remastered");
  CHECK(library.Find("c") == nullptr);
  CHECK(library.Games().size() == 2);
}

TEST_CASE("Removing scattered games, a source, or everything keeps each row where views expect") {
  GameLibraryModel library;
  // Aborts the test run on any row signal that disagrees with the model's contents.
  QAbstractItemModelTester tester(&library, QAbstractItemModelTester::FailureReportingMode::Fatal);
  library.Replace({Game("a", "A"), Game("b", "B", {}, "epic"), Game("c", "C"),
                   Game("d", "D", {}, "epic"), Game("e", "E"), Game("f", "F")});
  const auto ids = [&] {
    std::vector<std::string> out;
    for (const GameSummary& game : library.Games()) out.push_back(game.id);
    return out;
  };
  const auto findable = [&] {
    for (const GameSummary& game : library.Games()) {
      const QVariant id = library.IndexOf(game.id).data(GameTileDelegate::IdRole);
      if (id.toString().toStdString() != game.id) return false;
    }
    return true;
  };

  library.Remove({"a", "e", "c", "nope"});  // not adjacent, out of order, one unknown
  CHECK(ids() == std::vector<std::string>{"b", "d", "f"});
  CHECK(findable());

  library.RemoveSource("epic");
  CHECK(ids() == std::vector<std::string>{"f"});
  CHECK(findable());

  // A batch naming one new game twice adds it once, as its last copy says.
  library.Upsert({Game("g", "First"), Game("g", "Second")});
  CHECK(ids() == std::vector<std::string>{"f", "g"});
  CHECK(library.Find("g")->name == "Second");

  library.Remove({"f", "g"});
  CHECK(library.Games().empty());
  CHECK(library.Find("f") == nullptr);
}

TEST_CASE("Filters hide hidden games except under Hidden, and launchers everywhere") {
  GameLibraryModel library;
  library.Replace({Game("a", "Alpha"), Game("h", "Hushed", {"hidden"}),
                   Game("l", "Launcher", {}, "launcher")});
  GameFilterProxy proxy(&library);

  CHECK(Shown(proxy) == std::vector<std::string>{"a"});
  proxy.SetFilterKey("hidden");
  CHECK(Shown(proxy) == std::vector<std::string>{"h"});
}

TEST_CASE("Apps stay under All games, get their own filter, and never count as unplayed games") {
  GameLibraryModel library;
  library.Replace({Game("g", "Game"), Game("w", "Writer", {"app"})});
  GameFilterProxy proxy(&library);

  CHECK(Shown(proxy) == std::vector<std::string>{"g", "w"});
  proxy.SetFilterKey("apps");
  CHECK(Shown(proxy) == std::vector<std::string>{"w"});
  proxy.SetFilterKey("never");
  CHECK(Shown(proxy) == std::vector<std::string>{"g"});
}

TEST_CASE("Needs attention lists games mirad wants checked beside broken ones") {
  GameSummary unsure = Game("u", "Unsure");
  unsure.needs_check = true;
  GameSummary broken = Game("b", "Broken");
  broken.status = "broken";
  GameLibraryModel library;
  library.Replace({Game("f", "Fine"), unsure, broken});
  GameFilterProxy proxy(&library);

  proxy.SetFilterKey("attention");
  CHECK(Shown(proxy) == std::vector<std::string>{"b", "u"});
}

TEST_CASE("A source's view lists only that source's games, hidden ones included") {
  GameLibraryModel library;
  library.Replace({Game("a", "Alpha"), Game("e1", "Epic One", {}, "epic"), Game("e2", "Epic Two", {"hidden"}, "epic")});
  GameFilterProxy proxy(&library);
  proxy.SetSource("epic");
  CHECK(Shown(proxy) == std::vector<std::string>{"e1", "e2"});
}

TEST_CASE("Search, the sidebar sort, and running all follow the model's changes") {
  GameLibraryModel library;
  library.Replace({Game("b", "Beta"), Game("a", "Alpha"), Game("c", "Gamma")});
  GameFilterProxy proxy(&library);
  QAbstractItemModelTester tester(&proxy, QAbstractItemModelTester::FailureReportingMode::Fatal);

  CHECK(Shown(proxy) == std::vector<std::string>{"a", "b", "c"});
  proxy.SetSort("name", /*descending=*/true);
  CHECK(Shown(proxy) == std::vector<std::string>{"c", "b", "a"});

  proxy.SetSearch("  ga ");
  CHECK(Shown(proxy) == std::vector<std::string>{"c"});
  proxy.SetSearch("");

  proxy.SetFilterKey("running");
  CHECK(Shown(proxy).empty());
  library.SetRunning("a", true);
  CHECK(Shown(proxy) == std::vector<std::string>{"a"});
  library.Remove({"a"});
  CHECK(Shown(proxy).empty());
}
