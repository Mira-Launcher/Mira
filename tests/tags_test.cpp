#include <doctest.h>
#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <json.hpp>
#include <thread>

#include "metadata/MetadataFetcher.h"
#include "store/GameStore.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
using test::AwaitJob;
using test::LiveServer;
using test::TempDir;
using test::Touch;
using test::WaitUntil;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

model::Game AddGame(store::GameStore& games, const std::string& id, const fs::path& install_path,
                    std::vector<std::string> tags = {}, const std::string& runner_ref = "") {
  Touch(install_path / "run.sh", "#!/bin/sh\n", /*executable=*/true);
  model::Game game;
  game.id = id;
  game.name = id;
  game.source = "manual";
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.install_path = install_path.string();
  game.exe_path = "run.sh";
  game.runner_ref = runner_ref;
  game.tags = std::move(tags);
  REQUIRE(games.Upsert(game).has_value());
  return game;
}

// A metadata record as a fetch leaves it, with `steam_tags` when given.
void WriteMetadata(store::MetadataStore& cache, const std::string& id,
                   std::optional<std::vector<std::string>> steam_tags) {
  json info = {{"fetched_at", 1}, {"source", "steam"}};
  if (steam_tags) info["steam_tags"] = {{"appid", "1"}, {"tags", *steam_tags}};
  REQUIRE(cache.Write(id, info).has_value());
}

json Post(httplib::Client& client, const std::string& path, const json& body) {
  const auto res = client.Post(path, body.dump(), "application/json");
  REQUIRE(res != nullptr);
  INFO(res->body);
  REQUIRE(res->status == 200);
  return json::parse(res->body);
}

json GetTags(httplib::Client& client) {
  const auto res = client.Get("/v1/tags");
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 200);
  return json::parse(res->body);
}

const json* Named(const json& list, const std::string& name) {
  for (const json& entry : list) {
    if (entry["name"] == name) return &entry;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("The tag list counts your tags and suggests the Steam tags you don't have") {
  LiveServer server(TempDir("tags-list-state"));
  config::Config& config = server.MutableConfig();
  REQUIRE(config.Set("tags.steam", true).has_value());
  REQUIRE(config.Set("metadata.steam_by_name", true).has_value());
  REQUIRE(config.Set("tags.folders", json::array({"RPG", "Puzzle"})).has_value());
  const fs::path games = TempDir("tags-list-games");
  AddGame(server.games(), "a", games / "A", {"rpg", "favorite"});
  AddGame(server.games(), "b", games / "B", {"RPG", "Indie"});
  AddGame(server.games(), "c", games / "C", {}, "steam:3");
  AddGame(server.games(), "d", games / "D");
  WriteMetadata(server.games().Metadata(), "a", std::vector<std::string>{"RPG", "Indie", "Singleplayer"});
  WriteMetadata(server.games().Metadata(), "c", std::vector<std::string>{"Singleplayer", "Roguelike"});
  WriteMetadata(server.games().Metadata(), "d", std::nullopt);  // fetched before Steam tags were
  // A store's launcher: a Steam game of its name says nothing about it.
  model::Game launcher = AddGame(server.games(), "ea", games / "EA");
  launcher.source = "launcher";
  REQUIRE(server.games().Upsert(launcher).has_value());
  WriteMetadata(server.games().Metadata(), "ea", std::vector<std::string>{"Singleplayer", "Utilities"});
  httplib::Client client = server.Client();

  const json listed = GetTags(client);
  const json& mine = listed["tags"];
  REQUIRE(mine.size() == 3);
  CHECK(mine[0]["name"] == "rpg");  // most games first, spelled as first seen, any case counted
  CHECK(mine[0]["ids"] == json::array({"a", "b"}));
  CHECK(mine[0]["folder"] == true);
  CHECK(mine[0]["steam_ids"] == json::array({"a"}));
  CHECK(Named(mine, "Indie")->at("folder") == false);
  CHECK(Named(mine, "Puzzle")->at("count") == 0);  // a folder tag no game has yet
  CHECK(Named(mine, "favorite") == nullptr);

  const json& steam = listed["steam"];
  REQUIRE(steam.size() == 2);  // RPG and Indie are yours already
  CHECK(steam[0]["name"] == "Singleplayer");
  CHECK(steam[0]["ids"] == json::array({"a", "c"}));
  CHECK(steam[1]["name"] == "Roguelike");
  CHECK(listed["steam_missing"] == 1);

  REQUIRE(config.Set("tags.steam", false).has_value());
  CHECK(GetTags(client)["steam_missing"] == 0);
}

TEST_CASE("Setting, renaming and removing a tag across the library, with its folder") {
  LiveServer server(TempDir("tags-set-state"));
  config::Config& config = server.MutableConfig();
  const fs::path root = TempDir("tags-set-games");
  REQUIRE(config.Set("library_roots", json::array({root.string()})).has_value());
  REQUIRE(config.Set("tags.sorted_roots", json::array({root.string()})).has_value());
  AddGame(server.games(), "a", root / "A", {"Indie"});
  AddGame(server.games(), "b", root / "B", {"metroidvania", "Indie"});
  AddGame(server.games(), "c", root / "C");
  httplib::Client client = server.Client();

  // Exactly the listed games end with it: added to a and c, taken off b.
  const json set = Post(client, "/v1/tags/set",
                        {{"name", "Metroidvania"}, {"ids", {"a", "c"}}, {"folder", true}});
  CHECK(set["games"].size() == 3);
  CHECK(server.games().Find("a")->tags == std::vector<std::string>{"Indie", "Metroidvania"});
  CHECK(server.games().Find("b")->tags == std::vector<std::string>{"Indie"});
  CHECK(config.GetStringArray("tags.folders") == std::vector<std::string>{"Metroidvania"});
  CHECK(WaitUntil([&] { return fs::exists(root / "Metroidvania" / "A" / "run.sh"); }));
  CHECK(WaitUntil([&] { return fs::exists(root / "Metroidvania" / "C" / "run.sh"); }));

  // Renamed in place, merging a spelling a game already had, and the folder and a game's pick of
  // it follow.
  REQUIRE(server.games()
              .Update("c",
                      [](model::Game& game) {
                        game.tags.push_back("Platformer");
                        game.folder_tag = "Metroidvania";
                      })
              .has_value());
  Post(client, "/v1/tags/rename", {{"from", "metroidvania"}, {"to", "Platformer"}});
  CHECK(server.games().Find("a")->tags == std::vector<std::string>{"Indie", "Platformer"});
  CHECK(server.games().Find("c")->tags == std::vector<std::string>{"Platformer"});
  CHECK(server.games().Find("c")->folder_tag == "Platformer");
  CHECK(config.GetStringArray("tags.folders") == std::vector<std::string>{"Platformer"});
  CHECK(WaitUntil([&] { return fs::exists(root / "Platformer" / "A" / "run.sh"); }));
  CHECK(WaitUntil([&] { return !fs::exists(root / "Metroidvania"); }));

  // Removed from every game and from the folders; the games come back to the root level.
  Post(client, "/v1/tags/remove", {{"name", "platformer"}});
  CHECK(server.games().Find("a")->tags == std::vector<std::string>{"Indie"});
  CHECK(server.games().Find("c")->folder_tag.empty());
  CHECK(config.GetStringArray("tags.folders").empty());
  CHECK(WaitUntil(
      [&] { return fs::exists(root / "A" / "run.sh") && fs::exists(root / "C" / "run.sh"); }));

  const auto meaning = client.Post("/v1/tags/set", json{{"name", "hidden"}, {"ids", {"a"}}}.dump(),
                                   "application/json");
  REQUIRE(meaning != nullptr);
  CHECK(meaning->status == 400);
}

TEST_CASE("Fetching Steam tags fills in the games whose metadata has none") {
  LiveServer server(TempDir("tags-fetch-state"));
  config::Config& config = server.MutableConfig();
  REQUIRE(config.Set("tags.steam", true).has_value());
  REQUIRE(config.Set("metadata.steam_by_name", true).has_value());
  // Steam's three public endpoints, as a stand-in curl: the last argument is the URL.
  const fs::path bin = TempDir("tags-fetch-bin");
  Touch(bin / "curl",
        "#!/bin/sh\n"
        "for a; do url=\"$a\"; done\n"
        "echo \"$url\" >> \"$(dirname \"$0\")/requests.log\"\n"
        "case \"$url\" in\n"
        "*GetTagList*) echo '{\"response\":{\"tags\":[{\"tagid\":1,\"name\":\"Metroidvania\"},"
        "{\"tagid\":2,\"name\":\"Platformer\"}]}}' ;;\n"
        "*GetItems*) echo '{\"response\":{\"store_items\":[{\"appid\":10,\"success\":1,\"tags\":["
        "{\"tagid\":2,\"weight\":9},{\"tagid\":1,\"weight\":5}]},{\"appid\":20,\"success\":1,"
        "\"tags\":["
        "{\"tagid\":1,\"weight\":3}]}]}}' ;;\n"
        "*storesearch*) echo '{\"items\":[{\"type\":\"app\",\"id\":20,\"name\":\"Hollow Game\"}]}' "
        ";;\n"
        "esac\n",
        /*executable=*/true);
  test::PathPrepend path(bin);
  const fs::path games = TempDir("tags-fetch-games");
  AddGame(server.games(), "steam-10", games / "S", {}, "steam:10");
  AddGame(server.games(), "hollow", games / "H");
  REQUIRE(server.games()
              .Update("hollow", [](model::Game& game) { game.name = "Hollow Game"; })
              .has_value());
  AddGame(server.games(), "unknown", games / "U");
  AddGame(server.games(), "no-metadata", games / "N", {}, "steam:10");
  for (const char* id : {"steam-10", "hollow", "unknown"}) WriteMetadata(server.games().Metadata(), id, std::nullopt);
  REQUIRE(server.games()
              .Update("unknown", [](model::Game& game) { game.name = "Nothing Like It"; })
              .has_value());
  httplib::Client client = server.Client();

  CHECK(GetTags(client)["steam_missing"] == 3);
  const json job = AwaitJob(client, client.Post("/v1/tags/fetch"));
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["fetched"] == 3);  // the unmatched game counts: asked, and Steam has none

  const auto stored = metadata::StoredSteamTags(server.games().Metadata());
  CHECK(stored.at("steam-10") == std::vector<std::string>{"Platformer", "Metroidvania"});
  CHECK(stored.at("hollow") == std::vector<std::string>{"Metroidvania"});
  CHECK(stored.at("unknown") == std::vector<std::string>{});
  CHECK_FALSE(stored.contains("no-metadata"));  // its own fetch brings them
  const json listed = GetTags(client);
  CHECK(listed["steam_missing"] == 0);
  CHECK(Named(listed["steam"], "Metroidvania")->at("count") == 2);

  // Steam's tag names are kept: a later fetch asks only for the games' tags.
  AddGame(server.games(), "later", games / "L", {}, "steam:20");
  WriteMetadata(server.games().Metadata(), "later", std::nullopt);
  REQUIRE(AwaitJob(client, client.Post("/v1/tags/fetch"))["state"] == "finished");
  CHECK(metadata::StoredSteamTags(server.games().Metadata()).at("later") ==
        std::vector<std::string>{"Metroidvania"});
  std::ifstream log(bin / "requests.log");
  int name_lists = 0;
  for (std::string line; std::getline(log, line);)
    name_lists += line.find("GetTagList") != std::string::npos;
  CHECK(name_lists == 1);
}

TEST_CASE("The preview says which games a folder change would move, counting unsaved tags") {
  LiveServer server(TempDir("tags-preview-state"));
  const fs::path root = TempDir("tags-preview-games");
  REQUIRE(server.MutableConfig().Set("library_roots", json::array({root.string()})).has_value());
  AddGame(server.games(), "quest", root / "Quest", {"RPG"});
  AddGame(server.games(), "plain", root / "Plain", {"Indie"});
  httplib::Client client = server.Client();
  const json sorted = {{"folders", {"RPG"}}, {"sorted_roots", {root.string()}}};

  const json moving = Post(client, "/v1/tags/preview", sorted)["moving"];
  REQUIRE(moving.size() == 1);
  CHECK(moving[0]["id"] == "quest");
  CHECK(moving[0]["to"] == (root / "RPG" / "Quest").string());
  CHECK_FALSE(fs::exists(root / "RPG"));  // only a preview

  // A game's unsaved tags count as its tags: here plain is being given RPG.
  json with_edit = sorted;
  with_edit["tags"] = {{"plain", {"Indie", "RPG"}}};
  const json edited = Post(client, "/v1/tags/preview", with_edit)["moving"];
  REQUIRE(edited.size() == 2);
  const auto plain =
      std::ranges::find_if(edited, [](const json& move) { return move["id"] == "plain"; });
  REQUIRE(plain != edited.end());
  CHECK((*plain)["to"] == (root / "RPG" / "Plain").string());
  CHECK(server.games().Find("plain")->tags == std::vector<std::string>{"Indie"});

  const auto refused =
      client.Post("/v1/tags/preview", json{{"folders", {"hidden"}}}.dump(), "application/json");
  REQUIRE(refused != nullptr);
  CHECK(refused->status == 400);
}
