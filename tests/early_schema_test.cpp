#include <doctest.h>

#include <filesystem>

#include "migrate/EarlySchema.h"
#include "store/Database.h"
#include "store/GameStore.h"
#include "store/MetadataStore.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

TEST_CASE("Databases from builds before folder sorting get its columns and tables") {
  const fs::path dir = test::TempDir("early-schema");
  const fs::path library_db = dir / "mira.db";
  const fs::path cache_db = dir / "cache.db";
  {
    store::GameStore games(library_db);
    games.Load();
    model::Game game;
    game.id = "celeste";
    game.name = "Celeste";
    game.install_path = "/games/celeste";
    REQUIRE(games.Upsert(game).has_value());
  }
  // As those builds made them: the same schema step, without what came later.
  {
    store::Database library;
    REQUIRE(library.Open(library_db).has_value());
    REQUIRE(library.Exec("ALTER TABLE games DROP COLUMN library_link").has_value());
    REQUIRE(library.Exec("ALTER TABLE games DROP COLUMN folder_tag").has_value());
    store::Database cache;
    REQUIRE(cache.Open(cache_db).has_value());
    REQUIRE(cache.Exec("DROP TABLE lists").has_value());
  }

  migrate::RepairEarlySchemas(library_db, cache_db);
  migrate::RepairEarlySchemas(library_db, cache_db);  // and nothing more the next time

  store::GameStore games(library_db);
  games.Load();
  REQUIRE(games.Find("celeste").has_value());
  REQUIRE(games.Update("celeste", [](model::Game& game) { game.folder_tag = "RPG"; }).has_value());
  store::GameStore reread(library_db);
  reread.Load();
  CHECK(reread.Find("celeste")->folder_tag == "RPG");

  store::MetadataStore cache(dir);
  cache.Load();
  REQUIRE(cache.WriteList("steam_tag_names", nlohmann::json::array({"RPG"})).has_value());
  CHECK(cache.ReadList("steam_tag_names") == nlohmann::json::array({"RPG"}));
}
