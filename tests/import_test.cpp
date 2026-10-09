#include <doctest.h>

#include <filesystem>
#include <json.hpp>

#include "library/Import.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TestEnv;
using test::Touch;
namespace fs = std::filesystem;

TEST_CASE("import moves a dropped path into the right library root") {
  TestEnv env("import");
  const fs::path games = env.dir / "Games";
  const fs::path apps = env.dir / "Applications";
  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({games.string(), apps.string()})).has_value());

  const fs::path folder = env.dir / "dropped" / "Some Game";
  fs::create_directories(folder);
  const auto as_game = library::ImportInto(env.config, folder, library::ImportKind::kGame);
  REQUIRE(as_game.has_value());
  CHECK(*as_game == games / "Some Game");

  const fs::path file = env.dir / "dropped" / "tool.AppImage";
  Touch(file, "", true);
  const auto as_app = library::ImportInto(env.config, file, library::ImportKind::kApp);
  REQUIRE(as_app.has_value());
  CHECK(*as_app == apps / "tool.AppImage");

  const fs::path again = env.dir / "again" / "Some Game";
  fs::create_directories(again);
  const auto taken = library::ImportInto(env.config, again, library::ImportKind::kGame);
  REQUIRE_FALSE(taken.has_value());
  CHECK(taken.error().code == "target_exists");

  const auto inside = library::ImportInto(env.config, *as_game, library::ImportKind::kGame);
  REQUIRE_FALSE(inside.has_value());
  CHECK(inside.error().code == "already_in_library");
}
