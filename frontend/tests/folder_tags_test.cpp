#include <doctest.h>

#include <json.hpp>

#include <string>
#include <vector>

#include "client/JsonMapping.h"
#include "library/GameLibraryModel.h"

using nlohmann::json;
using namespace mira_gui;

namespace {

GameSummary Game(const std::string& id, std::vector<std::string> tags,
                 std::optional<std::vector<std::string>> folder_tags = std::nullopt) {
  GameSummary game;
  game.id = id;
  game.name = id;
  game.status = "ready";
  game.source = "scan";
  game.tags = std::move(tags);
  game.folder_tags = std::move(folder_tags);
  if (game.folder_tags) game.sort_root = "~/Mira/Games";
  return game;
}

}  // namespace

TEST_CASE("The folder mark follows the folder tags' order, or the game's own pick") {
  const std::optional<std::vector<std::string>> folders = std::vector<std::string>{"RPG", "Roguelike"};
  CHECK(FolderTagIndex({"Indie", "roguelike", "rpg"}, folders) == 2);  // ignoring case
  CHECK(FolderTagIndex({"Indie", "roguelike", "RPG"}, folders, "Roguelike") == 1);
  CHECK(FolderTagIndex({"Indie", "RPG"}, folders, "Roguelike") == 1);  // a pick it lacks is ignored
  CHECK(FolderTagIndex({"Indie", "RPG"}, folders, "Indie") == 1);  // so is one that isn't a folder
  CHECK(FolderTagIndex({"Indie", "favorite"}, folders) == -1);
  CHECK(FolderTagIndex({"RPG"}, std::nullopt) == -1);  // the folder isn't sorted
  // The tags Mira gives a meaning to are never a folder, even if one were listed.
  CHECK(FolderTagIndex({"hidden", "RPG"}, std::vector<std::string>{"hidden", "RPG"}) == 1);
}

TEST_CASE("A game's tags show in the library's tag order") {
  const std::vector<std::string> order = {"RPG", "Roguelike", "Indie", "Action"};
  CHECK(InTagOrder({"action", "favorite", "Mine", "rpg", "Indie"}, order) ==
        std::vector<std::string>{"rpg", "Indie", "action", "Mine", "favorite"});
}

TEST_CASE("The Tags list puts folder tags first, then the tags most games have") {
  const std::vector<std::string> folders = {"RPG", "Roguelike"};
  const std::vector<GameSummary> games = {
      Game("a", {"Indie", "RPG"}, folders),
      Game("b", {"rpg", "favorite"}, folders),
      Game("h", {"RPG", "Zany", "hidden"}, folders),
      Game("c", {"Action"}),
      Game("d", {"indie"}),
  };
  CHECK(TagOrder(games) == std::vector<std::string>{"RPG", "Roguelike", "Indie", "Action", "Zany"});
  const std::vector<LibraryTag> all = TagsUnder(games, "all", true);
  REQUIRE(all.size() == 4);
  CHECK(all[0].tag == "RPG");
  CHECK(all[0].folder);
  CHECK(all[0].count == 2);  // "rpg" counts with it, the hidden game doesn't
  CHECK(all[1].tag == "Roguelike");
  CHECK(all[1].count == 0);  // listed though no game has it yet
  CHECK(all[2].tag == "Indie");
  CHECK(all[2].count == 2);
  CHECK_FALSE(all[2].folder);
  CHECK(all[3].tag == "Action");

  const std::vector<LibraryTag> hidden = TagsUnder(games, "hidden", true);
  const auto zany = std::ranges::find_if(hidden, [](const LibraryTag& t) { return t.tag == "Zany"; });
  REQUIRE(zany != hidden.end());
  CHECK(zany->count == 1);
}

TEST_CASE("Records carry how their game is sorted") {
  const GameSummary sorted = mapping::ToGameSummary(
      json::parse(R"({"id": "a", "sort_root": "~/Mira/Games", "folder_tags": ["RPG", "Indie"]})"));
  CHECK(sorted.sort_root == "~/Mira/Games");
  REQUIRE(sorted.folder_tags.has_value());
  CHECK(*sorted.folder_tags == std::vector<std::string>{"RPG", "Indie"});

  const GameSummary store =
      mapping::ToGameSummary(json::parse(R"({"id": "s", "sort_root": null, "folder_tags": null})"));
  CHECK(store.sort_root.empty());
  CHECK_FALSE(store.folder_tags.has_value());
}
