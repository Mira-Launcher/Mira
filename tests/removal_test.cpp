#include <doctest.h>
#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <json.hpp>
#include <map>
#include <sstream>
#include <string>

#include "store/GameStore.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
using nlohmann::json;
using test::AwaitJob;
using test::LiveServer;
using test::TempDir;
using test::Touch;
namespace fs = std::filesystem;

namespace {

std::string ReadText(const fs::path& file) {
  std::ifstream in(file);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

void InstallSteamApp(const fs::path& steam, const std::string& appid, const std::string& name) {
  Touch(steam / "steamapps" / std::format("appmanifest_{}.acf", appid),
        std::format(R"("AppState" {{ "appid" "{0}" "name" "{1}" "installdir" "{1}" }})", appid,
                    name));
  Touch(steam / "steamapps" / "common" / name / "game", "", /*executable=*/true);
}

model::Game StoreGame(const std::string& source, const std::string& ref, const fs::path& folder) {
  model::Game game;
  game.id = source + "-" + ref;
  game.name = ref;
  game.source = source;
  game.source_ref = ref;
  game.install_path = folder.string();
  game.exe_path = "game.exe";
  game.status = model::GameStatus::Ready;
  return game;
}

}  // namespace

TEST_CASE("Uninstalling a Steam game hands it to Steam, and it leaves the library once Steam has") {
  const fs::path state = TempDir("removal-steam");
  const fs::path bin = state / "bin";
  Touch(bin / "steam", "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$0.args\"\n", /*executable=*/true);
  const test::PathPrepend path(bin);
  LiveServer server(state);
  const fs::path steam = server.config().GetPath("steam.root");
  InstallSteamApp(steam, "400", "Portal");
  httplib::Client client = server.Client();
  REQUIRE(AwaitJob(client, client.Post("/v1/steam/scan"))["state"] == "finished");
  REQUIRE(server.games()
              .Update("steam-400", [](model::Game& game) { game.tags.push_back("favorite"); })
              .has_value());

  const json handed = AwaitJob(client, client.Delete("/v1/games/steam-400?delete_files=true"));
  REQUIRE(handed["state"] == "finished");
  CHECK(handed["result"]["handed_to"] == "steam");
  CHECK(test::WaitUntil(
      [&] { return ReadText(bin / "steam.args").contains("steam://uninstall/400"); }));
  // Steam asks first, so nothing has gone yet.
  CHECK(server.games().Find("steam-400").has_value());
  CHECK(fs::exists(steam / "steamapps" / "common" / "Portal" / "game"));

  // Steam uninstalls it.
  fs::remove(steam / "steamapps" / "appmanifest_400.acf");
  fs::remove_all(steam / "steamapps" / "common" / "Portal");
  CHECK(
      test::WaitUntil([&] { return !server.games().Find("steam-400"); }, std::chrono::seconds(20)));

  // Installed again, it's what it was.
  InstallSteamApp(steam, "400", "Portal");
  REQUIRE(AwaitJob(client, client.Post("/v1/steam/scan"))["state"] == "finished");
  const auto back = server.games().Find("steam-400");
  REQUIRE(back.has_value());
  CHECK(std::ranges::contains(back->tags, std::string("favorite")));
}

TEST_CASE(
    "A store import moves a game uninstalled outside Mira out of the library, keeping its "
    "record, while one on a disk that's gone stays Missing") {
  const fs::path state = TempDir("removal-epic-sync");
  const fs::path legendary = state / "legendary";
  Touch(legendary, "#!/bin/sh\n[ \"$1\" = list-installed ] && cat \"$0.installed\"\nexit 0\n",
        /*executable=*/true);
  Touch(state / "legendary.installed", "[]");
  LiveServer server(state);
  REQUIRE(server.MutableConfig().Set("epic.legendary_bin", legendary.string()).has_value());
  const fs::path games_dir = state / "Games";
  fs::create_directories(games_dir);

  model::Game uninstalled = StoreGame("epic", "aaa", games_dir / "A");
  uninstalled.tags = {"favorite"};
  uninstalled.overrides = {{"launch.gamemode", true}};
  REQUIRE(server.games().Upsert(uninstalled).has_value());
  // Its folder's disk isn't mounted: the store can't list it either.
  REQUIRE(server.games().Upsert(StoreGame("epic", "bbb", state / "unmounted" / "B")).has_value());

  httplib::Client client = server.Client();
  REQUIRE(AwaitJob(client, client.Post("/v1/stores/epic/import"))["state"] == "finished");
  CHECK_FALSE(server.games().Find("epic-aaa").has_value());
  const auto removed = test::WaitForEvent(server.events(), "games.removed");
  REQUIRE(removed.has_value());
  CHECK(removed->payload["ids"] == json::array({"epic-aaa"}));
  REQUIRE(server.games().Find("epic-bbb").has_value());
  CHECK(server.games().Find("epic-bbb")->status == model::GameStatus::Missing);

  // Installed again through the store's own client.
  Touch(games_dir / "A" / "game.exe");
  Touch(state / "legendary.installed", json::array({{{"app_name", "aaa"},
                                                     {"title", "aaa"},
                                                     {"install_path", (games_dir / "A").string()},
                                                     {"executable", "game.exe"}}})
                                           .dump());
  REQUIRE(AwaitJob(client, client.Post("/v1/stores/epic/import"))["state"] == "finished");
  const auto back = server.games().Find("epic-aaa");
  REQUIRE(back.has_value());
  CHECK(std::ranges::contains(back->tags, std::string("favorite")));
  CHECK(back->overrides == uninstalled.overrides);
}

TEST_CASE(
    "Removing a game keeps only the art the user picked, a store game's info stays for its "
    "store list, and forgetting it drops everything") {
  LiveServer server(TempDir("removal-art"));
  store::MetadataStore& metadata = server.games().Metadata();
  const auto write_art = [&](const std::string& id) {
    Touch(metadata.ArtworkDir(id) / "cover.chosen.png", "picked");
    Touch(metadata.ArtworkDir(id) / "hero.jpg", "fetched");
    const json info = {
        {"description", "fetched"},
        {"artwork",
         {{"file", "cover.chosen.png"}, {"content_type", "image/png"}, {"chosen", true}}},
        {"hero", {{"file", "hero.jpg"}}}};
    REQUIRE(metadata.Write(id, info).has_value());
  };
  model::Game celeste;
  celeste.id = "celeste";
  celeste.name = "Celeste";
  celeste.install_path = "/games/celeste";
  REQUIRE(server.games().Upsert(celeste).has_value());
  REQUIRE(server.games().Upsert(StoreGame("gog", "12", "/games/gog")).has_value());
  write_art("celeste");
  write_art("gog-12");

  httplib::Client client = server.Client();
  REQUIRE(client.Delete("/v1/games/celeste")->status == 200);
  CHECK(metadata.ArtFor("celeste", "cover").has_value());
  CHECK_FALSE(metadata.ArtFor("celeste", "hero").has_value());
  CHECK_FALSE(fs::exists(metadata.ArtworkDir("celeste") / "hero.jpg"));
  CHECK_FALSE(metadata.Read("celeste").contains("description"));

  REQUIRE(client.Delete("/v1/games/gog-12")->status == 200);
  CHECK(metadata.ArtFor("gog-12", "hero").has_value());
  CHECK(metadata.Read("gog-12").contains("description"));

  REQUIRE(server.games().Upsert(celeste).has_value());
  REQUIRE(client.Delete("/v1/games/celeste?delete_metadata=true")->status == 200);
  CHECK_FALSE(metadata.ArtFor("celeste", "cover").has_value());
  REQUIRE(server.games().Upsert(celeste).has_value());
  CHECK(server.games().Find("celeste")->tags.empty());
}

TEST_CASE("A desktop entry's prefix Mira made can be deleted, but never the app's own files") {
  LiveServer server(TempDir("removal-desktop-entry"));
  const fs::path prefix = server.config().GetPath("prefix_root") / "notepad";
  const fs::path app = TempDir("removal-desktop-entry-app");
  Touch(prefix / "system.reg", "reg");
  Touch(app / "notepad.exe");
  model::Game entry;
  entry.id = "notepad";
  entry.name = "Notepad";
  entry.source = "desktop-entry";
  entry.install_path = app.string();
  entry.exe_path = "notepad.exe";
  entry.data_dir = prefix.string();
  REQUIRE(server.games().Upsert(entry).has_value());

  httplib::Client client = server.Client();
  const json done =
      AwaitJob(client, client.Delete("/v1/games/notepad?delete_files=true&delete_prefix=true"));
  CHECK(done["state"] == "finished");
  CHECK_FALSE(fs::exists(prefix));
  CHECK(fs::exists(app / "notepad.exe"));
}

TEST_CASE("The removal plan says what each kind of game loses, and how much") {
  LiveServer server(TempDir("removal-plan"));
  const fs::path library_root = TempDir("removal-plan-library");
  REQUIRE(server.MutableConfig()
              .Set("library_roots", nlohmann::json::array({library_root.string()}))
              .has_value());
  const fs::path prefixes = server.config().GetPath("prefix_root");
  const auto add = [&](model::Game game) {
    REQUIRE(server.games().Upsert(game).has_value());
    return game;
  };

  model::Game steam = StoreGame("steam", "400", TempDir("removal-plan-steam") / "Portal");
  steam.id = "steam-400";
  add(steam);
  model::Game epic = StoreGame("epic", "aaa", TempDir("removal-plan-epic") / "A");
  epic.data_dir = (prefixes / "epic-aaa").string();
  Touch(prefixes / "epic-aaa" / "system.reg", std::string(100, 'r'));
  add(epic);

  model::Game folder;
  folder.id = "animal-well";
  folder.name = "Animal Well";
  folder.install_path = (library_root / "Animal Well").string();
  folder.data_dir = (prefixes / "animal-well").string();
  Touch(library_root / "Animal Well" / "game.exe", std::string(30, 'x'));
  Touch(prefixes / "animal-well" / "system.reg", std::string(10, 'r'));
  add(folder);

  model::Game inside;
  inside.id = "dispatch";
  inside.name = "Dispatch";
  inside.data_dir = (prefixes / "dispatch").string();
  inside.install_path = (prefixes / "dispatch" / "drive_c" / "Games" / "Dispatch").string();
  Touch(fs::path(inside.install_path) / "game.exe", std::string(40, 'x'));
  Touch(prefixes / "dispatch" / "system.reg", std::string(5, 'r'));
  add(inside);

  model::Game own_prefix;
  own_prefix.id = "cuphead";
  own_prefix.name = "Cuphead";
  own_prefix.data_dir = (prefixes / "cuphead").string();
  own_prefix.install_path = own_prefix.data_dir;
  Touch(prefixes / "cuphead" / "Cuphead.exe");
  add(own_prefix);

  model::Game entry;
  entry.id = "olympus";
  entry.name = "Olympus";
  entry.source = "desktop-entry";
  entry.install_path = TempDir("removal-plan-olympus").string();
  add(entry);
  // A game Mira didn't install, outside every Mira folder.
  model::Game outside;
  outside.id = "outside";
  outside.name = "Outside";
  outside.install_path = TempDir("removal-plan-outside").string();
  add(outside);
  // Sharing Animal Well's prefix keeps it.
  model::Game sharer = folder;
  sharer.id = "sharer";
  sharer.name = "Sharer";
  sharer.install_path = (library_root / "Sharer").string();

  httplib::Client client = server.Client();
  const json ids = {"steam-400", "epic-aaa", "animal-well", "dispatch",
                    "cuphead",   "olympus",  "outside",     "nope"};
  const auto plans = [&] {
    auto res = client.Post("/v1/games/removal", json{{"ids", ids}}.dump(), "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    std::map<std::string, json> by_id;
    for (const json& plan : json::parse(res->body)) by_id[plan["id"]] = plan;
    return by_id;
  };
  std::map<std::string, json> plan = plans();
  CHECK(plan.size() == 7);
  CHECK(plan["steam-400"]["kind"] == "steam");
  CHECK(plan["steam-400"]["prefix"] == "");
  CHECK(plan["epic-aaa"]["kind"] == "store");
  CHECK(plan["epic-aaa"]["tool"] == "Legendary");
  CHECK(plan["epic-aaa"]["prefix"] == epic.data_dir);
  CHECK(plan["animal-well"]["kind"] == "folder");
  CHECK(plan["animal-well"]["files"] == folder.install_path);
  CHECK(plan["animal-well"]["files_in_prefix"] == "");
  CHECK(plan["dispatch"]["kind"] == "folder");
  CHECK(plan["dispatch"]["files_in_prefix"] == "C:\\Games\\Dispatch");
  CHECK(plan["cuphead"]["kind"] == "folder");
  CHECK(plan["cuphead"]["folder_is_prefix"] == true);
  CHECK(plan["olympus"]["kind"] == "external");
  CHECK(plan["outside"]["kind"] == "external");
  CHECK(plan["animal-well"]["prefix_shared_with"] == "");
  add(sharer);
  CHECK(plans()["animal-well"]["prefix_shared_with"] == "Sharer");

  const json sizes = AwaitJob(client, client.Post("/v1/games/removal/sizes",
                                                  json{{"ids", ids}}.dump(), "application/json"));
  REQUIRE(sizes["state"] == "finished");
  CHECK(sizes["result"]["animal-well"]["files"] == 30);
  CHECK(sizes["result"]["animal-well"]["prefix"] == 10);
  // The rest of the prefix, without the game inside it.
  CHECK(sizes["result"]["dispatch"]["files"] == 40);
  CHECK(sizes["result"]["dispatch"]["prefix"] == 5);
  CHECK(sizes["result"]["epic-aaa"]["prefix"] == 100);

  // A folder that's its prefix goes as the prefix.
  const json deleted = AwaitJob(client, client.Delete("/v1/games/cuphead?delete_files=true"));
  CHECK(deleted["state"] == "finished");
  CHECK_FALSE(fs::exists(prefixes / "cuphead"));
}
