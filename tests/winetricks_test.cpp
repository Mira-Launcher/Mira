#include "runner/Winetricks.h"

#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>

#include "config/Config.h"
#include "model/Types.h"
#include "runner/Exec.h"
#include "runner/RunnerRegistry.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {
fs::path TempFile(const char* name) {
  return test::TestsRoot() / name;
}
}  // namespace

TEST_CASE("RunTricksVerb rejects a native runner outright") {
  config::Config config(TempFile("tricks-native-settings.toml"));
  config.Load();
  test::Isolate(config);
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
  test::Isolate(config);
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
  test::Isolate(config);
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

  // A Steam game's data_dir is its compatdata folder, with the prefix inside it as pfx.
  model::Game steam = game;
  steam.data_dir = (env.dir / "compatdata" / "2379780").string();
  const fs::path pfx = fs::path(steam.data_dir) / "pfx";
  fs::create_directories(pfx / "drive_c");
  REQUIRE(runner::RunTricksVerb(registry, steam, "corefonts"));
  in.clear();
  std::string steam_run;
  std::getline(in, steam_run);
  CHECK(steam_run.find("|" + pfx.string() + "|") != std::string::npos);

  // Steam's own redistributable put Visual C++ 14.51 there: vcrun2022 has nothing to add, and its
  // installer would refuse. An older runtime is still upgraded.
  fs::create_directories(pfx / "drive_c" / "windows" / "syswow64");
  const auto runtime = [&](const char* minor) {
    std::string reg = "WINE REGISTRY Version 2\n#arch=win64\n\n";
    for (const char* key :
         {"Software\\\\Microsoft\\\\VisualStudio\\\\14.0\\\\VC\\\\Runtimes\\\\x64",
          "Software\\\\Wow6432Node\\\\Microsoft\\\\VisualStudio\\\\14.0\\\\VC\\\\"
          "Runtimes\\\\x86"}) {
      reg += std::format(
          "[{}] 1790973961\n\"Installed\"=dword:00000001\n\"Major\"=dword:0000000e\n"
          "\"Minor\"=dword:{}\n\n",
          key, minor);
    }
    test::Touch(pfx / "system.reg", reg);
  };
  runtime("00000033");
  REQUIRE(runner::RunTricksVerb(registry, steam, "vcrun2022"));
  in.clear();
  std::string skipped;
  CHECK_FALSE(std::getline(in, skipped));
  runtime("00000014");
  REQUIRE(runner::RunTricksVerb(registry, steam, "vcrun2022"));
  in.clear();
  std::string upgraded;
  REQUIRE(std::getline(in, upgraded));
  CHECK(upgraded.ends_with("vcrun2022"));
}
