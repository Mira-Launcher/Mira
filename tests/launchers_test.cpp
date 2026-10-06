#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "launchers/Launchers.h"
#include "runner/WindowsTheme.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

TEST_CASE("launchers: Ubisoft installs are read from system.reg") {
  const fs::path prefix = fs::temp_directory_path() / "mira-tests" / "launcher-reg";
  fs::remove_all(prefix);
  fs::create_directories(prefix / "drive_c");
  std::ofstream(prefix / "system.reg")
      << "WINE REGISTRY Version 2\n\n"
         "[Software\\\\Wow6432Node\\\\Ubisoft\\\\Launcher\\\\Installs\\\\5595] 1700000000\n"
         "\"InstallDir\"=\"C:/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/Trackmania/\"\n\n"
         "[Software\\\\Wow6432Node\\\\Ubisoft\\\\Launcher\\\\Installs\\\\5595\\\\Nested] 1700000000\n"
         "\"InstallDir\"=\"C:\\\\nope\"\n";

  const auto installs =
      launchers::ReadRegSubkeys(prefix / "system.reg", "Software\\Wow6432Node\\Ubisoft\\Launcher\\Installs");
  REQUIRE(installs.size() == 1);
  const std::string dir = installs.at("5595").at("InstallDir");
  CHECK(launchers::HostPath(prefix, dir) ==
        prefix / "drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games/Trackmania");
}

TEST_CASE("launchers: Battle.net and EA games are found in their prefixes, and go missing") {
  test::TestEnv env("launcher-import");
  const fs::path battlenet = env.dir / "prefixes" / "battle-net";
  const fs::path ea = env.dir / "prefixes" / "ea";
  test::Touch(battlenet / "drive_c" / "Program Files (x86)" / "Hearthstone" / "Hearthstone.exe");
  test::Touch(battlenet / "drive_c" / "Program Files" / "Diablo IV" / "Diablo IV.exe");
  const fs::path titanfall = ea / "drive_c" / "Program Files" / "EA Games" / "Titanfall2";
  test::Touch(titanfall / "Titanfall2.exe");
  test::Touch(titanfall / "__Installer" / "installerdata.xml",
              "<DiPManifest>"
              "<contentIDs><contentID>1063734</contentID><contentID>1065733</contentID>"
              "</contentIDs>"
              "<gameTitles><gameTitle locale=\"en_US\">Titanfall 2</gameTitle></gameTitles>"
              "</DiPManifest>");

  for (const auto& [id, prefix] : {std::pair{"battlenet", battlenet}, std::pair{"ea", ea}}) {
    model::Game host;
    host.id = "launcher-" + std::string(id);
    host.source = "launcher";
    host.source_ref = id;
    host.data_dir = prefix.string();
    host.status = model::GameStatus::Ready;
    REQUIRE(env.games.Upsert(host));
  }

  const auto import = [&env](const char* launcher) {
    return launchers::Import(env.config, env.games, env.events, *launchers::Find(launcher));
  };
  const auto blizzard = import("battlenet");
  REQUIRE(blizzard);
  CHECK(blizzard->added == 2);
  const auto hearthstone = env.games.Find("battlenet-wtcg");
  REQUIRE(hearthstone);
  CHECK(hearthstone->name == "Hearthstone");
  CHECK(hearthstone->source_ref == "WTCG");
  CHECK(hearthstone->exe_path == "Hearthstone.exe");
  CHECK(env.games.Find("battlenet-fen"));

  REQUIRE(import("ea"));
  const auto game = env.games.Find("ea-titanfall2");
  REQUIRE(game);
  CHECK(game->name == "Titanfall 2");
  CHECK(game->source_ref == "1063734,1065733");

  // Uninstalled through Battle.net itself.
  fs::remove_all(battlenet / "drive_c" / "Program Files" / "Diablo IV");
  REQUIRE(import("battlenet"));
  CHECK(env.games.Find("battlenet-fen")->status == model::GameStatus::Missing);
  CHECK(env.games.Find("battlenet-wtcg")->status == model::GameStatus::Ready);

  // Nothing to import from a launcher that isn't installed.
  CHECK_FALSE(import("ubisoft"));
}

TEST_CASE("launchers: games are matched by their folder as Wine shows it") {
  model::Game game;
  game.data_dir = "/games/prefixes/battle-net";
  game.install_path = "/games/prefixes/battle-net/drive_c/Program Files (x86)/Hearthstone";
  CHECK(launchers::WindowsDir(game) == "c:/program files (x86)/hearthstone");
  game.install_path = "/mnt/Games/Trackmania/";
  CHECK(launchers::WindowsDir(game) == "z:/mnt/games/trackmania");
}

TEST_CASE("launchers: Microsoft 365 apps are imported as apps with their own exe") {
  test::TestEnv env("launcher-office");
  const fs::path prefix = env.dir / "prefixes" / "microsoft-365";
  const fs::path office16 = prefix / "drive_c" / "Program Files" / "Microsoft Office" / "root" / "Office16";
  test::Touch(office16 / "EXCEL.EXE");
  test::Touch(office16 / "WINWORD.EXE");

  model::Game host;
  host.id = "launcher-office";
  host.source = "launcher";
  host.source_ref = "office";
  host.data_dir = prefix.string();
  host.status = model::GameStatus::Ready;
  REQUIRE(env.games.Upsert(host));

  const auto imported = launchers::Import(env.config, env.games, env.events, *launchers::Find("office"));
  REQUIRE(imported);
  CHECK(imported->added == 2);
  const auto excel = env.games.Find("office-excel");
  REQUIRE(excel);
  CHECK(excel->name == "Excel");
  CHECK(excel->exe_path == "EXCEL.EXE");
  CHECK(std::ranges::find(excel->tags, "app") != excel->tags.end());
  CHECK_FALSE(env.games.Find("office-powerpoint"));
}

TEST_CASE("Windows theme: the desktop's color scheme and a prefix's light/dark setting are read") {
  CHECK(runner::ParseColorScheme("v u 1\n") == std::optional(true));
  CHECK(runner::ParseColorScheme("(<<uint32 2>>,)\n") == std::optional(false));
  CHECK_FALSE(runner::ParseColorScheme("v u 0\n").has_value());

  const std::string reg =
      "[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Themes\\\\Personalize] 1791\n#time=1\n"
      "\"AppsUseLightTheme\"=dword:00000000\n\n[Software\\\\Wine] 1\n";
  CHECK(runner::PrefixPrefersDark(reg) == std::optional(true));
  CHECK_FALSE(runner::PrefixPrefersDark("[Software\\\\Wine] 1\n").has_value());
}
