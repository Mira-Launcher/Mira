#include <doctest.h>

#include <atomic>
#include <fstream>
#include <filesystem>
#include <thread>
#include <vector>

#include "store/GameStore.h"

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

TEST_CASE("a finished session is counted once and goes with its game") {
  store::GameStore store(TempFile("games-sessions.db"));
  store.Load();
  REQUIRE(store.Upsert(MakeGame("celeste", "Celeste")).has_value());
  const store::PlaySession session{.game_id = "celeste", .started_at = 100, .ended_at = 160, .duration_seconds = 60};
  for (int i = 0; i < 2; ++i) {
    REQUIRE(store.FinishSession(session, [](model::Game& game) { game.play_seconds += 60; }).has_value());
  }
  CHECK(store.Sessions("celeste", 10).size() == 1);
  REQUIRE(store.Remove("celeste").has_value());
  REQUIRE(store.Upsert(MakeGame("celeste", "Celeste")).has_value());
  CHECK(store.Sessions("celeste", 10).empty());
}

TEST_CASE("a SaveBatch's changes reach the database together when it ends") {
  const fs::path file = TempFile("games-batch.db");
  store::GameStore store(file);
  store.Load();
  {
    const auto batch = store.BatchSaves();
    REQUIRE(store.Upsert(MakeGame("celeste", "Celeste")).has_value());
    REQUIRE(store.Upsert(MakeGame("hades", "Hades")).has_value());
    CHECK(store.All().size() == 2);
    store::GameStore meanwhile(file);
    meanwhile.Load();
    CHECK(meanwhile.All().empty());
  }
  store::GameStore reloaded(file);
  reloaded.Load();
  CHECK(reloaded.All().size() == 2);
}

TEST_CASE("games.toml is imported once, keeping its games' tags") {
  const fs::path file = TempFile("games-import.db");
  const fs::path toml = file.parent_path() / "games.toml";
  {
    std::ofstream out(toml);
    out << "[[game]]\nid = 'celeste'\nname = 'Celeste'\nstatus = 'ready'\ntags = ['hidden', 'favorite']\n"
           "[[game]]\nid = 'hades'\nname = 'Hades'\nstatus = 'ready'\n";
  }
  store::GameStore store(file);
  store.Load();
  CHECK(store.All().size() == 2);
  CHECK(store.Find("celeste")->tags == std::vector<std::string>{"hidden", "favorite"});
  CHECK_FALSE(fs::exists(toml));
  CHECK(fs::exists(toml.string() + ".migrated"));
  fs::remove(toml.string() + ".migrated");
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
        auto result = store.Update("g" + std::to_string(i), [](model::Game& game) { game.tags = {"favorite"}; });
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
