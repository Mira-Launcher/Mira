#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "amazon/AmazonImporter.h"
#include "api/EventBus.h"
#include "config/Config.h"
#include "store/GameStore.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

TEST_CASE("amazon: installed games launch from their fuel.json") {
  test::TestEnv env("amazon-import");
  const fs::path dir = env.dir;
  fs::create_directories(dir / "nile");
  fs::create_directories(dir / "games/Some Game");
  ::setenv("NILE_CONFIG_PATH", dir.c_str(), 1);

  std::ofstream(dir / "nile/installed.json")
      << R"([{"id": "amzn1.adg.product.abc", "version": "1", "path": ")"
      << (dir / "games/Some Game").string() << R"("}])";
  std::ofstream(dir / "nile/library.json")
      << R"([{"id": "ent-1", "product": {"id": "amzn1.adg.product.abc", "title": "Some Game", "sku": "SKU1"}}])";
  std::ofstream(dir / "games/Some Game/fuel.json")
      << "{ // json5 comment\n \"SchemaVersion\": \"2\", \"Main\": {\"Command\": "
         "\"bin\\\\Game.exe\", "
         "\"Args\": [\"-fuel\"], \"WorkingSubdirOverride\": \"bin\"}}";

  amazon::AmazonImporter importer(env.config, env.games, env.events);
  const auto summary = importer.Import();
  ::unsetenv("NILE_CONFIG_PATH");
  REQUIRE(summary);
  CHECK(summary->added == 1);

  const auto game = env.games.Find("amazon-amzn1.adg.product.abc");
  REQUIRE(game);
  CHECK(game->name == "Some Game");
  CHECK(game->exe_path == "bin/Game.exe");
  CHECK(game->args == "-fuel");
  CHECK(game->working_dir == "bin");
  CHECK(game->env.at("AMAZON_GAMES_FUEL_ENTITLEMENT_ID") == "ent-1");
  CHECK(game->env.at("AMAZON_GAMES_FUEL_PRODUCT_SKU") == "SKU1");
}
