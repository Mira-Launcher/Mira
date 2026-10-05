#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "config/Config.h"
#include "model/Types.h"
#include "runner/Exec.h"
#include "runner/RunnerRegistry.h"
#include "runner/Winetricks.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {
fs::path TempFile(const char* name) { return fs::temp_directory_path() / "mira-tests" / name; }
}  // namespace

TEST_CASE("RunTricksVerb rejects a native runner outright") {
  config::Config config(TempFile("tricks-native-settings.toml"));
  config.Load();
  const runner::RunnerRegistry registry(config);

  model::Game game;
  game.id = "native-game";
  game.data_dir = TempFile("tricks-native-prefix").string();
  fs::create_directories(fs::path(game.data_dir) / "drive_c");
  game.runner_ref = "native:native";

  const auto ran = runner::RunTricksVerb(registry, game, "corefonts");
  REQUIRE_FALSE(ran.has_value());
  CHECK(ran.error().code == "not_wine_based");

  fs::remove_all(game.data_dir);
}

TEST_CASE("RunTricksVerb rejects a game with no data_dir") {
  config::Config config(TempFile("tricks-nodatadir-settings.toml"));
  config.Load();
  const runner::RunnerRegistry registry(config);

  model::Game game;
  game.id = "no-prefix-game";
  game.runner_ref = "wine:system";

  const auto ran = runner::RunTricksVerb(registry, game, "corefonts");
  REQUIRE_FALSE(ran.has_value());
  CHECK(ran.error().code == "no_data_dir");
}

TEST_CASE("RunTricksVerb rejects an unprovisioned prefix") {
  config::Config config(TempFile("tricks-unprovisioned-settings.toml"));
  config.Load();
  const runner::RunnerRegistry registry(config);

  const fs::path data_dir = TempFile("tricks-unprovisioned-prefix");
  fs::remove_all(data_dir);
  fs::create_directories(data_dir);  // exists, but no drive_c -- never provisioned

  model::Game game;
  game.id = "unprovisioned-game";
  game.data_dir = data_dir.string();
  game.runner_ref = "wine:system";

  const auto ran = runner::RunTricksVerb(registry, game, "corefonts");
  REQUIRE_FALSE(ran.has_value());
  CHECK(ran.error().code == "not_provisioned");

  fs::remove_all(data_dir);
}

TEST_CASE("RunTricksVerb runs winetricks unattended against the game's own Wine and prefix") {
  test::TestEnv env("tricks-run");
  const fs::path wine = env.dir / "runners" / "wine" / "wine-9.0-amd64" / "bin" / "wine";
  test::Touch(wine, "#!/bin/sh\necho wine-9.0\n", /*executable=*/true);
  test::Touch(wine.parent_path() / "wineserver", "", /*executable=*/true);
  const fs::path bin = env.dir / "bin";
  const fs::path log = env.dir / "winetricks.log";
  test::Touch(bin / "winetricks",
              "#!/bin/sh\necho \"$WINE|$WINESERVER|$WINEPREFIX|$*\" >> '" + log.string() +
                  "'\n[ \"$2\" = broken ] && exit 3\nexit 0\n",
              /*executable=*/true);
  const test::PathPrepend path(bin);

  model::Game game;
  game.id = "celeste";
  game.runner_ref = "wine:wine-9.0-amd64";
  game.data_dir = (env.dir / "prefixes" / "celeste").string();
  fs::create_directories(fs::path(game.data_dir) / "drive_c");
  const runner::RunnerRegistry registry(env.config);

  REQUIRE(runner::RunTricksVerb(registry, game, "corefonts"));
  const auto failed = runner::RunTricksVerb(registry, game, "broken");
  REQUIRE_FALSE(failed);
  CHECK(failed.error().code == "tricks_failed");
  // Anything that could pass as an option or a second command never reaches winetricks.
  for (const char* verb : {"-q", "corefonts;rm", "a b", ""}) {
    CHECK(runner::RunTricksVerb(registry, game, verb).error().code == "invalid_verb");
  }

  std::ifstream in(log);
  std::string first;
  std::getline(in, first);
  const std::string wineserver = (wine.parent_path() / "wineserver").string();
  CHECK(first ==
        wine.string() + "|" + wineserver + "|" + game.data_dir + "|--unattended corefonts");
  std::string second, third;
  std::getline(in, second);
  CHECK_FALSE(std::getline(in, third));  // the two valid verbs, nothing else
}
