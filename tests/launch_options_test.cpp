#include <doctest.h>

#include "library/LaunchOptions.h"

using namespace mira;
using nlohmann::json;

TEST_CASE("Launch options are checked, given ids, and picked as the default") {
  CHECK(library::LaunchProblem(json{{"options", json::array({{{"name", "A"}, {"exe", "a"}, {"mode", "x"}}})}}));
  CHECK(library::LaunchProblem(json{{"options", json::array({{{"name", "A"}, {"exe", "a"}, {"id", "game"}}})}}));
  CHECK(library::LaunchProblem(json{{"options", json::array({{{"exe", "a"}}})}}));

  const json launch = library::WithLaunchIds(
      {{"default", "mod-tool"},
       {"options", json::array({{{"name", "Mod Tool"}, {"exe", "/t"}}, {{"name", "Mod Tool"}, {"exe", "/u"}, {"mode", "tool"}}})}});
  CHECK_FALSE(library::LaunchProblem(launch));
  CHECK(launch["options"][0]["id"] == "mod-tool");
  CHECK(launch["options"][1]["id"] == "mod-tool-2");

  model::Game game;
  game.launch = launch;
  REQUIRE(library::DefaultLaunch(game));
  CHECK(library::DefaultLaunch(game)->exe == "/t");
  game.launch["default"] = "mod-tool-2";  // a tool can't be what Play starts
  CHECK_FALSE(library::DefaultLaunch(game));
}

TEST_CASE("Launch placeholders fill in the game's paths, as Wine paths for a prefix") {
  model::Game game;
  game.install_path = "/games/My Game";
  game.exe_path = "bin/game.exe";
  game.data_dir = "/prefixes/my-game";
  CHECK(library::FillLaunchPlaceholders("--dir={game_dir}", game, false) == "--dir=/games/My Game");
  CHECK(library::FillLaunchPlaceholders("{game_exe}", game, true) == "Z:\\games\\My Game\\bin\\game.exe");
  CHECK(library::FillLaunchPlaceholders("{prefix}{prefix}", game, false) == "/prefixes/my-game/prefixes/my-game");
}
