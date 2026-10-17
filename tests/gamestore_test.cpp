#include "store/GameStore.h"

#include <doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

#include "store/Database.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {
fs::path TempFile(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests";
  fs::create_directories(dir);
  const fs::path file = dir / name;
  for (const char* suffix : {"", "-wal", "-shm", ".bak"}) fs::remove(file.string() + suffix);
  return file;
}

model::Game MakeGame(const std::string& id, const std::string& name) {
  model::Game game;
  game.id = id;
  game.name = name;
  game.install_path = "/games/" + id;
  game.status = model::GameStatus::Ready;
  return game;
}
}  // namespace

TEST_CASE("NextId disambiguates collisions so ids stay readable") {
  store::GameStore store(TempFile("games-ids.db"));
  store.Load();
  CHECK(store.NextId("Celeste") == "celeste");
  REQUIRE(store.Upsert(MakeGame("celeste", "Celeste")).has_value());
  CHECK(store.NextId("Celeste") == "celeste-2");
}

TEST_CASE("games round-trip through the database, including nested fields") {
  const fs::path file = TempFile("games-roundtrip.db");
  store::GameStore store(file);
  store.Load();

  model::Game game = MakeGame("celeste", "Celeste");
  game.candidates.push_back({"Celeste.exe", model::Platform::Windows, 4.5, true});
  game.env["FOO"] = "bar";
  game.overrides["scan.max_depth"] = 8;
  game.library_link = "/games/RPG/Celeste";
  game.folder_tag = "RPG";
  REQUIRE(store.Upsert(game).has_value());

  store::GameStore reloaded(file);
  reloaded.Load();
  auto found = reloaded.Find("celeste");
  REQUIRE(found.has_value());
  CHECK(found->name == "Celeste");
  CHECK(found->candidates.size() == 1);
  CHECK(found->candidates[0].chosen);
  CHECK(found->env.at("FOO") == "bar");
  CHECK(found->overrides.value("scan.max_depth", 0) == 8);
  CHECK(found->library_link == "/games/RPG/Celeste");
  CHECK(found->folder_tag == "RPG");
}

TEST_CASE("Update mutates under lock and reports the saved result") {
  store::GameStore store(TempFile("games-update.db"));
  store.Load();
  REQUIRE(store.Upsert(MakeGame("celeste", "Celeste")).has_value());

  auto result = store.Update("celeste", [](model::Game& game) {
    game.reviewed = true;
    game.play_seconds += 60;
  });
  REQUIRE(result.has_value());
  CHECK(result->reviewed);
  CHECK(result->play_seconds == 60);

  auto missing = store.Update("no-such-id", [](model::Game&) {});
  CHECK_FALSE(missing.has_value());
}

TEST_CASE("UpdateMany saves every changed game and skips unknown or unchanged ones") {
  const auto file = TempFile("games-update-many.db");
  store::GameStore store(file);
  store.Load();
  REQUIRE(store.Upsert(MakeGame("celeste", "Celeste")).has_value());
  REQUIRE(store.Upsert(MakeGame("hades", "Hades")).has_value());
  REQUIRE(store.Upsert(MakeGame("tunic", "Tunic")).has_value());

  auto result = store.UpdateMany({"celeste", "hades", "no-such-id"}, [](model::Game& game) {
    if (game.id == "hades") return false;
    game.tags.push_back("hidden");
    return true;
  });
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 1);
  CHECK(result->front().id == "celeste");

  store::GameStore reloaded(file);
  reloaded.Load();
  CHECK(reloaded.Find("celeste")->tags == std::vector<std::string>{"hidden"});
  CHECK(reloaded.Find("hades")->tags.empty());
  CHECK(reloaded.Find("tunic")->tags.empty());
}

TEST_CASE("Remove deletes a game and reports an error for an unknown id") {
  store::GameStore store(TempFile("games-remove.db"));
  store.Load();
  REQUIRE(store.Upsert(MakeGame("celeste", "Celeste")).has_value());
  CHECK(store.Remove("celeste").has_value());
  CHECK_FALSE(store.Find("celeste").has_value());
  CHECK_FALSE(store.Remove("celeste").has_value());
}

TEST_CASE("hidden store titles survive a reload, and only store games leave one behind") {
  const fs::path file = TempFile("games-hidden-titles.db");
  store::GameStore store(file);
  store.Load();
  model::Game store_game = MakeGame("gog-12", "Store Game");
  store_game.source = "gog";
  store_game.source_ref = "12";
  store_game.tags = {"hidden"};
  model::Game local = MakeGame("celeste", "Celeste");
  local.tags = {"hidden"};
  REQUIRE(store.Upsert(store_game).has_value());
  REQUIRE(store.Upsert(local).has_value());
  REQUIRE(store.SetTitlesHidden({"epic-abc"}, true).has_value());
  REQUIRE(store.RemoveMany({"gog-12", "celeste"}).has_value());

  store::GameStore reloaded(file);
  reloaded.Load();
  CHECK(reloaded.HiddenTitles() == std::set<std::string>{"epic-abc", "gog-12"});
}

TEST_CASE("a finished session is counted once and outlives its game") {
  store::GameStore store(TempFile("games-sessions.db"));
  store.Load();
  REQUIRE(store.Upsert(MakeGame("celeste", "Celeste")).has_value());
  const store::PlaySession session{
      .game_id = "celeste", .started_at = 100, .ended_at = 160, .duration_seconds = 60};
  for (int i = 0; i < 2; ++i) {
    REQUIRE(store.FinishSession(session, [](model::Game& game) { game.play_seconds += 60; })
                .has_value());
  }
  CHECK(store.Sessions("celeste", 10).size() == 1);
  REQUIRE(store.Remove("celeste").has_value());
  CHECK(store.NextId("Celeste") == "celeste-2");  // a new game doesn't take its history
  REQUIRE(store.Upsert(MakeGame("celeste", "Celeste")).has_value());
  CHECK(store.Sessions("celeste", 10).size() == 1);
  CHECK(store.Find("celeste")->play_seconds == 60);
}

TEST_CASE("a removed game takes back what the user set on it when it's added again") {
  const fs::path dir = test::TempDir("gamestore-kept");
  const fs::path prefix = dir / "prefixes" / "epic-abc";
  fs::create_directories(prefix);
  test::Touch(dir / "AGame" / "Picked.exe");
  test::Touch(dir / "AGame" / "Store.exe");
  store::GameStore store(dir / "games.db");
  store.Load();

  model::Game game = MakeGame("epic-abc", "My Name");
  game.source = "epic";
  game.source_ref = "abc";
  game.install_path = (dir / "AGame").string();
  game.exe_path = "Picked.exe";
  game.runner_ref = "wine:custom";
  game.data_dir = prefix.string();
  game.overrides = {{"launch.gamemode", true}};
  game.launch = {{"options", {{{"name", "Safe mode"}}}}};
  game.tags = {"favorite", "Rpg", "hidden"};
  REQUIRE(store.Upsert(game).has_value());
  REQUIRE(store.Remove("epic-abc").has_value());
  CHECK(store.KeptDataDir("epic-abc") == prefix.string());

  // As a store import adds it back: its own name, exe and a new prefix path.
  model::Game found = MakeGame("epic-abc", "A Game");
  found.source = "epic";
  found.source_ref = "abc";
  found.install_path = (dir / "AGame").string();
  found.exe_path = "Store.exe";
  found.data_dir = (dir / "prefixes" / "new").string();
  REQUIRE(store.Upsert(found).has_value());
  const model::Game back = *store.Find("epic-abc");
  CHECK(back.name == "My Name");
  CHECK(back.exe_path == "Picked.exe");
  CHECK(back.runner_ref == "wine:custom");
  CHECK(back.data_dir == prefix.string());
  CHECK(back.overrides == game.overrides);
  CHECK(back.launch == game.launch);
  // Its title stayed hidden while removed, so it comes back hidden too.
  CHECK(std::set(back.tags.begin(), back.tags.end()) ==
        std::set<std::string>{"favorite", "Rpg", "hidden"});

  // A picked exe that's gone isn't put back, and a forgotten game keeps nothing.
  fs::remove(dir / "AGame" / "Picked.exe");
  REQUIRE(store.Remove("epic-abc").has_value());
  REQUIRE(store.Upsert(found).has_value());
  CHECK(store.Find("epic-abc")->exe_path == "Store.exe");
  CHECK(store.Find("epic-abc")->name == "My Name");
  REQUIRE(store.Remove("epic-abc", /*forget=*/true).has_value());
  CHECK(store.KeptDataDir("epic-abc").empty());
  REQUIRE(store.Upsert(found).has_value());
  CHECK(store.Find("epic-abc")->name == "A Game");
  CHECK(store.Find("epic-abc")->overrides.empty());
}

TEST_CASE("a game found again where a removed one was gets the removed one's id") {
  store::GameStore store(TempFile("games-kept-ids.db"));
  store.Load();
  model::Game celeste = MakeGame("celeste", "Celeste");
  celeste.exe_path = "Celeste.exe";
  model::Game appimage = MakeGame("tool", "Tool");
  appimage.install_path = "/games/Apps";
  appimage.exe_path = "Tool.AppImage";
  REQUIRE(store.Upsert(celeste).has_value());
  REQUIRE(store.Upsert(appimage).has_value());
  REQUIRE(store.RemoveMany({"celeste", "tool"}).has_value());

  CHECK(store.NextId("Celeste Classic", "/games/celeste") == "celeste");
  CHECK(store.NextId("Tool Box", "/games/Apps", "Tool.AppImage") == "tool");
  // Another program in the same folder is another game.
  CHECK(store.NextId("Tool Box", "/games/Apps", "Other.AppImage") == "tool-box");
  REQUIRE(store.Upsert(MakeGame("celeste", "Celeste")).has_value());
  CHECK(store.NextId("Celeste Classic", "/games/celeste") == "celeste-classic");
}

TEST_CASE("an older library's tags are cased as Steam's, merging tags that only differed in case") {
  const fs::path file = TempFile("games-tag-case.db");
  {
    store::GameStore store(file);
    store.Load();
    model::Game game = MakeGame("celeste", "Celeste");
    game.tags = {"rpg", "Rpg", "open world", "eSports", "favorite"};
    REQUIRE(store.Upsert(game).has_value());
  }
  {
    store::Database db;
    REQUIRE(db.Open(file).has_value());
    REQUIRE(db.Exec("PRAGMA user_version = 6").has_value());  // from before the step
  }
  store::GameStore store(file);
  store.Load();
  CHECK(store.Find("celeste")->tags ==
        std::vector<std::string>{"Rpg", "Open World", "eSports", "favorite"});
}

TEST_CASE("a damaged database is set aside and the library comes back from its backup") {
  const fs::path file = TempFile("games-damaged.db");
  {
    store::GameStore store(file);
    store.Load();
    REQUIRE(store.Upsert(MakeGame("celeste", "Celeste")).has_value());
  }
  store::GameStore(file).Load();  // backs up the library with celeste in it
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << "not a database";
  }
  store::GameStore store(file);
  store.Load();
  CHECK(store.Find("celeste").has_value());
  CHECK(fs::exists(file.string() + ".bad"));
  CHECK(store.Upsert(MakeGame("hades", "Hades")).has_value());
  fs::remove(file.string() + ".bad");
}

TEST_CASE("concurrent updates all save, and the file ends with every change") {
  // The API serves requests on several threads, and batch actions send one per game.
  const fs::path file = TempFile("games-concurrent.db");
  store::GameStore store(file);
  store.Load();
  constexpr int kGames = 8;
  for (int i = 0; i < kGames; ++i) {
    REQUIRE(store.Upsert(MakeGame("g" + std::to_string(i), "Game")).has_value());
  }

  std::atomic<int> failures = 0;
  std::vector<std::thread> threads;
  for (int i = 0; i < kGames; ++i) {
    threads.emplace_back([&store, &failures, i] {
      for (int round = 0; round < 10; ++round) {
        auto result = store.Update("g" + std::to_string(i),
                                   [](model::Game& game) { game.tags = {"favorite"}; });
        if (!result) ++failures;
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  CHECK(failures == 0);

  store::GameStore reloaded(file);
  reloaded.Load();
  for (int i = 0; i < kGames; ++i) {
    auto found = reloaded.Find("g" + std::to_string(i));
    REQUIRE(found.has_value());
    CHECK(found->tags == std::vector<std::string>{"favorite"});
  }
}
