#include <doctest.h>
#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <json.hpp>
#include <thread>

#include "library/FolderTags.h"
#include "library/Scanner.h"
#include "library/Watcher.h"
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

// The settings that sort `root` by `folders`, as a merge-patch of the settings.
json Sorting(const fs::path& root, const std::vector<std::string>& folders) {
  return {{"tags", {{"folders", folders}, {"sorted_roots", json::array({root.string()})}}}};
}


model::Game AddGame(store::GameStore& games, const std::string& id, const fs::path& install_path,
                    std::vector<std::string> tags = {}, const std::string& source = "manual") {
  Touch(install_path / "run.sh", "#!/bin/sh\n", /*executable=*/true);
  model::Game game;
  game.id = id;
  game.name = id;
  game.source = source;
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.install_path = install_path.string();
  game.exe_path = "run.sh";
  game.tags = std::move(tags);
  REQUIRE(games.Upsert(game).has_value());
  return game;
}

json PatchGame(httplib::Client& client, const std::string& id, const json& patch) {
  const auto res = client.Patch("/v1/games/" + id, patch.dump(), "application/json");
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 200);
  return json::parse(res->body);
}

void SetTags(httplib::Client& client, const std::string& id, const std::vector<std::string>& tags) {
  PatchGame(client, id, {{"tags", tags}});
}

// Waits for the game to be stored at `install_path`, with its program there.
bool MovedTo(store::GameStore& games, const std::string& id, const fs::path& install_path) {
  return WaitUntil([&] {
    const auto game = games.Find(id);
    return game && game->install_path == install_path.string() &&
           fs::exists(install_path / game->exe_path);
  });
}

}  // namespace

TEST_CASE("A game's folder follows its tags in a library folder sorted by tag") {
  LiveServer server(TempDir("folder-tags-follow-state"));
  const fs::path games_root = TempDir("folder-tags-follow-games");
  const fs::path apps_root = TempDir("folder-tags-follow-apps");
  config::Config& config = server.MutableConfig();
  REQUIRE(config.Set("library_roots", json::array({games_root.string(), apps_root.string()}))
              .has_value());
  REQUIRE(
      config.Patch(Sorting(games_root, {"RPG", "Strategy"})).has_value());
  AddGame(server.games(), "quest", games_root / "Quest");
  AddGame(server.games(), "editor", apps_root / "Editor");
  AddGame(server.games(), "steam-game", games_root / "SteamGame", {}, "steam");
  httplib::Client client = server.Client();

  SetTags(client, "quest", {"Indie", "rpg"});  // tags match folder tags ignoring case
  CHECK(MovedTo(server.games(), "quest", games_root / "RPG" / "Quest"));
  CHECK_FALSE(fs::exists(games_root / "Quest"));

  // tags.folders' order picks the folder, not the game's own order.
  CHECK(PatchGame(client, "quest", {{"tags", {"Strategy", "RPG"}}})["folder"] == "RPG");
  CHECK(MovedTo(server.games(), "quest", games_root / "RPG" / "Quest"));

  // The game's own pick wins over it; the folder left empty goes.
  CHECK(PatchGame(client, "quest", {{"folder_tag", "Strategy"}})["folder"] == "Strategy");
  CHECK(MovedTo(server.games(), "quest", games_root / "Strategy" / "Quest"));
  // Pruned just after the game is stored at its new place.
  CHECK(WaitUntil([&] { return !fs::exists(games_root / "RPG"); }));

  SetTags(client, "quest", {"Strategy", "RPG", "hidden"});
  CHECK(MovedTo(server.games(), "quest", games_root / ".hidden" / "Strategy" / "Quest"));
  CHECK(WaitUntil([&] { return !fs::exists(games_root / "Strategy"); }));

  // Losing the tag drops the pick, so having it again follows the order.
  SetTags(client, "quest", {});
  CHECK(MovedTo(server.games(), "quest", games_root / "Quest"));
  CHECK(WaitUntil([&] { return !fs::exists(games_root / ".hidden"); }));
  SetTags(client, "quest", {"Strategy", "RPG"});
  CHECK(MovedTo(server.games(), "quest", games_root / "RPG" / "Quest"));
  SetTags(client, "quest", {});
  REQUIRE(MovedTo(server.games(), "quest", games_root / "Quest"));

  // Only folders listed in the setting are sorted, and store installs never move.
  SetTags(client, "editor", {"RPG", "hidden"});
  SetTags(client, "steam-game", {"RPG"});
  SetTags(client, "quest", {"RPG"});
  CHECK(MovedTo(server.games(), "quest", games_root / "RPG" / "Quest"));
  CHECK(server.games().Find("editor")->install_path == (apps_root / "Editor").string());
  CHECK(server.games().Find("steam-game")->install_path == (games_root / "SteamGame").string());
}

TEST_CASE("Sorting by tag: batch folder_tag, a taken target, and moving the whole library") {
  LiveServer server(TempDir("folder-tags-batch-state"));
  const fs::path root = TempDir("folder-tags-batch-games");
  config::Config& config = server.MutableConfig();
  REQUIRE(config.Set("library_roots", json::array({root.string()})).has_value());
  REQUIRE(config.Patch(Sorting(root, {"RPG", "Strategy"})).has_value());
  AddGame(server.games(), "alpha", root / "alpha", {"Strategy"});
  AddGame(server.games(), "beta", root / "beta");
  httplib::Client client = server.Client();
  REQUIRE(
      MovedTo(server.games(), "alpha", root / "alpha"));  // nothing sorts until something changes

  const auto res = client.Patch("/v1/games", R"({"ids": ["alpha", "beta"], "folder_tag": "RPG"})",
                                "application/json");
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 200);
  CHECK(server.games().Find("alpha")->tags == std::vector<std::string>{"Strategy", "RPG"});
  CHECK(server.games().Find("alpha")->folder_tag == "RPG");
  CHECK(MovedTo(server.games(), "alpha", root / "RPG" / "alpha"));
  CHECK(MovedTo(server.games(), "beta", root / "RPG" / "beta"));

  // Without a pick, reordering tags.folders moves the game to its new first one.
  PatchGame(client, "alpha", {{"folder_tag", ""}});
  const auto ordered = client.Patch("/v1/config", R"({"tags": {"folders": ["Strategy", "RPG"]}})",
                                    "application/json");
  REQUIRE(ordered != nullptr);
  REQUIRE(ordered->status == 200);
  CHECK(MovedTo(server.games(), "alpha", root / "Strategy" / "alpha"));
  CHECK(MovedTo(server.games(), "beta", root / "RPG" / "beta"));
  SetTags(client, "alpha", {"RPG"});
  REQUIRE(MovedTo(server.games(), "alpha", root / "RPG" / "alpha"));

  // The library-wide move keeps games in their sorting folder instead of flattening them.
  const auto relocated = AwaitJob(client, client.Post("/v1/library/relocate"));
  REQUIRE(relocated["state"] == "finished");
  CHECK(relocated["result"]["moved"] == 0);
  CHECK(server.games().Find("alpha")->install_path == (root / "RPG" / "alpha").string());

  // A folder already at the target stops the move, and the game stays where it is.
  Touch(root / "Strategy" / "alpha" / "notes.txt");
  SetTags(client, "alpha", {"Strategy"});
  const auto failed = test::WaitForEvent(server.events(), "job.failed");
  REQUIRE(failed.has_value());
  CHECK(failed->payload["error"]["code"] == "target_exists");
  CHECK(server.games().Find("alpha")->install_path == (root / "RPG" / "alpha").string());
  CHECK(fs::exists(root / "RPG" / "alpha" / "run.sh"));
}

TEST_CASE("A running game is sorted once it exits") {
  LiveServer server(TempDir("folder-tags-running-state"));
  const fs::path root = TempDir("folder-tags-running-games");
  config::Config& config = server.MutableConfig();
  REQUIRE(config.Set("library_roots", json::array({root.string()})).has_value());
  REQUIRE(config.Patch(Sorting(root, {"RPG"})).has_value());
  AddGame(server.games(), "quest", root / "Quest");
  Touch(root / "Quest" / "run.sh", "#!/bin/sh\nsleep 1\n", /*executable=*/true);
  httplib::Client client = server.Client();

  const auto launched = client.Post("/v1/games/quest/launch");
  REQUIRE(launched != nullptr);
  REQUIRE(WaitUntil([&] { return server.services().supervisor.IsRunning("quest"); }));
  SetTags(client, "quest", {"RPG"});
  // Nothing moves while it runs, so the job finishes having moved nothing.
  const auto finished = test::WaitForEvent(server.events(), "job.finished");
  REQUIRE(finished.has_value());
  CHECK(finished->payload["result"]["moved"].empty());
  CHECK(server.games().Find("quest")->install_path == (root / "Quest").string());

  CHECK(MovedTo(server.games(), "quest", root / "RPG" / "Quest"));
}

TEST_CASE("A scan finds games in sorting folders, tags them by place, and leaves empty ones") {
  test::TestEnv env("folder-tags-scan-state");
  const fs::path root = TempDir("folder-tags-scan-games");
  REQUIRE(env.config.Set("library_roots", json::array({root.string()})).has_value());
  REQUIRE(env.config.Patch(Sorting(root, {"RPG", "Strategy"})).has_value());
  const std::string root_tag = root.filename().string();
  for (const fs::path& game : {root / "RPG" / "Alpha", root / ".hidden" / "RPG" / "Beta",
                               root / ".hidden" / "Gamma", root / "Other" / "Delta"}) {
    Touch(game / "run.sh", "#!/bin/sh\n", /*executable=*/true);
  }
  library::Scanner scanner(env.config, env.games, env.events);
  scanner.ScanAll();

  CHECK(env.games.Find("alpha")->tags == std::vector<std::string>{"RPG", root_tag});
  CHECK(env.games.Find("beta")->tags == std::vector<std::string>{"RPG", root_tag, "hidden"});
  CHECK(env.games.Find("gamma")->tags == std::vector<std::string>{root_tag, "hidden"});
  CHECK(env.games.Find("other").has_value());  // not a sorting folder, so a game of its own
  CHECK_FALSE(env.games.Find("delta").has_value());

  // A nested game whose folder is gone is missing, like one at the root level.
  fs::remove_all(root / ".hidden" / "Gamma");
  CHECK(scanner.ScanAll().missing == 1);
  CHECK(env.games.Find("gamma")->status == model::GameStatus::Missing);
  // So is one whose whole sorting folder was deleted with it.
  fs::remove_all(root / "RPG");
  CHECK(scanner.ScanAll().missing == 1);
  CHECK(env.games.Find("alpha")->status == model::GameStatus::Missing);

  // A sorting folder made by hand isn't pruned by a scan, so games can be dragged into it.
  fs::create_directories(root / "Strategy");
  scanner.ScanAll();
  CHECK(fs::exists(root / "Strategy"));
}

TEST_CASE("Pruning removes a sorting folder only when it holds no game and nothing but leftovers") {
  test::TestEnv env("folder-tags-prune-state");
  const fs::path root = TempDir("folder-tags-prune-games");
  REQUIRE(env.config.Set("library_roots", json::array({root.string()})).has_value());
  REQUIRE(env.config.Patch(Sorting(root, {"RPG", "Strategy"})).has_value());
  Touch(root / ".hidden" / "RPG" / ".directory");
  Touch(root / "Strategy" / "notes.txt");
  const model::Game kept = AddGame(env.games, "kept", root / "RPG" / "Kept");

  library::PruneEmptyContainers(env.config, root / ".hidden" / "RPG", env.games.All());
  CHECK_FALSE(fs::exists(root / ".hidden"));  // .hidden too, once empty
  library::PruneEmptyContainers(env.config, root / "Strategy", env.games.All());
  CHECK(fs::exists(root / "Strategy" / "notes.txt"));
  library::PruneEmptyContainers(env.config, root / "RPG", env.games.All());
  CHECK(fs::exists(root / "RPG" / "Kept"));
  library::PruneEmptyContainers(env.config, root, env.games.All());
  CHECK(fs::exists(root));
}

TEST_CASE("The watcher finds games dropped into a sorting folder") {
  test::TestEnv env("folder-tags-watch-state");
  const fs::path root = TempDir("folder-tags-watch-games");
  REQUIRE(env.config.Set("library_roots", json::array({root.string()})).has_value());
  REQUIRE(env.config.Patch(Sorting(root, {"RPG"})).has_value());
  REQUIRE(env.config.Set("scan.debounce_ms", 100).has_value());
  Touch(root / "RPG" / ".directory");
  library::Watcher watcher(env.config, env.games, env.events);
  std::thread watcher_thread([&] { watcher.Run(); });
  REQUIRE(WaitUntil([&] { return watcher.RootsWatched() > 0; }));

  Touch(root / "RPG" / "Beta" / "run.sh", "#!/bin/sh\n", /*executable=*/true);
  CHECK(WaitUntil([&] { return env.games.Find("beta").has_value(); }));

  watcher.Stop();
  watcher_thread.join();
}

TEST_CASE("A folder linked into a library folder by hand is a game, scan after scan") {
  test::TestEnv env("folder-tags-own-link-state");
  const fs::path root = TempDir("folder-tags-own-link-games");
  const fs::path elsewhere = TempDir("folder-tags-own-link-elsewhere");
  REQUIRE(env.config.Set("library_roots", json::array({root.string()})).has_value());
  REQUIRE(env.config.Patch(Sorting(root, {"RPG"})).has_value());
  REQUIRE(env.config.Set("library.remove_missing", true).has_value());
  Touch(elsewhere / "Witcher" / "run.sh", "#!/bin/sh\n", /*executable=*/true);
  fs::create_directory_symlink(elsewhere / "Witcher", root / "Witcher");
  library::Scanner scanner(env.config, env.games, env.events);

  scanner.ScanAll();
  REQUIRE(env.games.Find("witcher").has_value());
  CHECK(env.games.Find("witcher")->install_path == (root / "Witcher").string());
  CHECK(scanner.ScanAll().missing == 0);
  CHECK(env.games.Find("witcher").has_value());
}

TEST_CASE("A hand edit of the sorting settings sorts the library as a request would") {
  LiveServer server(TempDir("folder-tags-hand-edit-state"));
  const fs::path root = TempDir("folder-tags-hand-edit-games");
  REQUIRE(server.MutableConfig().Set("library_roots", json::array({root.string()})).has_value());
  AddGame(server.games(), "quest", root / "Quest", {"RPG"});

  // Another writer of settings.toml, as an editor would be; mirad's watcher then reloads.
  config::Config editor(server.MutableConfig().File());
  editor.Load();
  REQUIRE(editor.Patch(Sorting(root, {"RPG"})).has_value());
  server.services().ReloadSettings();
  CHECK(MovedTo(server.games(), "quest", root / "RPG" / "Quest"));
}

TEST_CASE("A game installed in its prefix is sorted by a link the scanner never follows") {
  LiveServer server(TempDir("folder-tags-link-state"));
  const fs::path root = TempDir("folder-tags-link-games");
  const fs::path prefixes = TempDir("folder-tags-link-prefixes");
  config::Config& config = server.MutableConfig();
  REQUIRE(config.Set("library_roots", json::array({root.string()})).has_value());
  REQUIRE(config.Set("prefix_root", prefixes.string()).has_value());
  const fs::path install = prefixes / "quest" / "drive_c" / "Games" / "Quest";
  model::Game quest = AddGame(server.games(), "quest", install);
  REQUIRE(server.games()
              .Update("quest",
                      [&](model::Game& game) {
                        game.name = "Quest";
                        game.data_dir = (prefixes / "quest").string();
                      })
              .has_value());
  httplib::Client client = server.Client();

  // Turning sorting on links it at the root level; nothing appears before that, and turning it
  // off (null, as a merge-patch) takes the link away again.
  CHECK_FALSE(fs::exists(root / "Quest"));
  const auto patched = client.Patch(
      "/v1/config", Sorting(root, {"RPG"}).dump(),
      "application/json");
  REQUIRE(patched != nullptr);
  REQUIRE(patched->status == 200);
  CHECK(WaitUntil([&] { return fs::is_symlink(root / "Quest"); }));
  CHECK(fs::read_symlink(root / "Quest") == install);
  const auto off = client.Patch(
      "/v1/config", json{{"tags", {{"sorted_roots", json::array()}}}}.dump(), "application/json");
  REQUIRE(off != nullptr);
  REQUIRE(off->status == 200);
  CHECK(WaitUntil([&] { return !fs::exists(fs::symlink_status(root / "Quest")); }));
  REQUIRE(client.Patch("/v1/config", Sorting(root, {"RPG"}).dump(), "application/json")->status ==
          200);
  CHECK(WaitUntil([&] { return fs::is_symlink(root / "Quest"); }));

  SetTags(client, "quest", {"RPG"});
  CHECK(WaitUntil([&] {
    return fs::is_symlink(root / "RPG" / "Quest") &&
           !fs::exists(fs::symlink_status(root / "Quest"));
  }));
  CHECK(server.games().Find("quest")->install_path ==
        install.string());  // the game itself never moves
  CHECK(server.games().Find("quest")->library_link == (root / "RPG" / "Quest").string());

  // A scan neither adds the link as a game nor looks inside it.
  const std::size_t before = server.games().All().size();
  REQUIRE(AwaitJob(client, client.Post("/v1/library/scan"))["state"] == "finished");
  CHECK(server.games().All().size() == before);

  // Removing the game takes the link with it, and the files stay.
  const auto removed = client.Delete("/v1/games/quest");
  REQUIRE(removed != nullptr);
  CHECK(WaitUntil([&] { return !fs::exists(fs::symlink_status(root / "RPG" / "Quest")); }));
  CHECK(fs::exists(install / "run.sh"));
}

TEST_CASE("A game's own link left pointing at its old folder is replaced") {
  test::TestEnv env("folder-tags-stale-link-state");
  const fs::path root = TempDir("folder-tags-stale-link-games");
  const fs::path prefixes = TempDir("folder-tags-stale-link-prefixes");
  REQUIRE(env.config.Set("library_roots", json::array({root.string()})).has_value());
  REQUIRE(env.config.Set("prefix_root", prefixes.string()).has_value());
  REQUIRE(env.config.Patch(Sorting(root, {"RPG"})).has_value());
  const fs::path before = prefixes / "quest" / "drive_c" / "Games" / "Quest";
  const fs::path after = prefixes / "quest" / "drive_c" / "Games" / "Quest Reinstalled";
  Touch(before / "run.sh", "#!/bin/sh\n", /*executable=*/true);
  Touch(after / "run.sh", "#!/bin/sh\n", /*executable=*/true);
  model::Game game;
  game.id = "quest";
  game.name = "Quest";
  game.source = "manual";
  game.install_path = before.string();
  game.exe_path = "run.sh";
  game.data_dir = (prefixes / "quest").string();
  game.tags = {"RPG"};
  auto placed = library::Place(env.config, game, {});
  REQUIRE(placed.has_value());
  REQUIRE(placed->library_link == (root / "RPG" / "Quest").string());

  placed->install_path = after.string();
  auto again = library::Place(env.config, *placed, {});
  REQUIRE(again.has_value());
  CHECK(fs::read_symlink(root / "RPG" / "Quest") == after);
}

TEST_CASE("Records say how their game is sorted") {
  LiveServer server(TempDir("folder-tags-records-state"));
  const fs::path root = TempDir("folder-tags-records-games");
  REQUIRE(server.MutableConfig().Set("library_roots", json::array({root.string()})).has_value());
  AddGame(server.games(), "quest", root / "Quest", {"RPG"});
  AddGame(server.games(), "plain", root / "Plain", {"Indie"});
  AddGame(server.games(), "store", root / "Store", {"RPG"}, "steam");
  httplib::Client client = server.Client();

  const json quest = json::parse(client.Get("/v1/games/quest")->body);
  CHECK(quest["sort_root"] == root.string());
  CHECK(quest["folder_tags"].is_null());  // not sorted yet
  // Turning sorting on tells clients every record changed, not only the games that move.
  const auto patched = client.Patch("/v1/config", Sorting(root, {"RPG"}).dump(),
                                    "application/json");
  REQUIRE(patched != nullptr);
  REQUIRE(patched->status == 200);
  const auto announced = test::WaitForEvent(server.events(), "games.updated");
  REQUIRE(announced.has_value());
  const json& records = announced->payload["games"];
  const auto plain = std::ranges::find_if(records, [](const json& game) { return game["id"] == "plain"; });
  REQUIRE(plain != records.end());
  CHECK((*plain)["folder_tags"] == json::array({"RPG"}));
  CHECK(json::parse(client.Get("/v1/games/quest")->body)["folder_tags"] == json::array({"RPG"}));
  CHECK(json::parse(client.Get("/v1/games/store")->body)["sort_root"].is_null());
}

TEST_CASE("tags.folders refuses tags that can't be folders") {
  test::TestEnv env("folder-tags-validate-state");
  CHECK(env.config.Set("tags.folders", json::array({"RPG", "Indie"})).has_value());
  CHECK(env.config.Set("tags.folders", json::array()).has_value());
  for (const char* bad : {"hidden", "Favorite", "app", ".secret", "a/b", ""}) {
    CAPTURE(bad);
    CHECK_FALSE(env.config.Set("tags.folders", json::array({bad})).has_value());
  }
  CHECK_FALSE(env.config.Set("tags.folders", json::array({"RPG", "rpg"})).has_value());
}
