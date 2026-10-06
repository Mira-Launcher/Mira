#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "config/Config.h"
#include "launchers/Launchers.h"
#include "launchers/Office.h"
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

TEST_CASE("launchers: Office's install percent comes from Click-to-Run's log, UTF-16 or not") {
  const fs::path prefix = fs::temp_directory_path() / "mira-tests" / "office-percent";
  fs::remove_all(prefix);
  const fs::path temp = prefix / "drive_c/users/steamuser/AppData/Local/Temp";
  fs::create_directories(temp);
  const auto line = [](int percent) {
    return "10/06/2026 14:49:46.611\tOFFICECL\tClick-To-Run\taqvqm\tMedium\t"
           "ScenarioController::UpdateScenarioProgress - {4CCD26FB-A773-42FB-8E44-CD53798BC0E7}=" +
           std::to_string(percent) + " \n";
  };
  // Timestamps come from the kernel's coarse clock, which can trail now() by a few milliseconds.
  const auto since = fs::file_time_type::clock::now() - std::chrono::seconds(10);
  CHECK_FALSE(launchers::office::InstallPercent(prefix, since).has_value());

  std::ofstream(temp / "ascii.log") << line(3) << line(11);
  CHECK(launchers::office::InstallPercent(prefix, since) == 11);

  {
    std::ofstream wide(temp / "wide.log", std::ios::binary);
    for (const char c : line(42)) wide.put(c).put('\0');
  }
  CHECK(launchers::office::InstallPercent(prefix, since) == 42);  // the newer log wins

  // A log from before the installer started is an earlier run's.
  CHECK_FALSE(launchers::office::InstallPercent(prefix, since + std::chrono::hours(1)).has_value());
}

TEST_CASE("launchers: Office installs only the apps picked, and refuses an empty or unknown pick") {
  const fs::path state = test::TempDir("office-apps");
  config::Config config(state / "settings.toml");
  config.Load();

  // The default is everything: nothing the five apps are named for is excluded.
  const std::string all = launchers::office::Configuration(config);
  for (const char* app : {"Word", "Excel", "PowerPoint", "Outlook", "OneNote"}) {
    CHECK(all.find(std::string("<ExcludeApp ID=\"") + app + "\"/>") == std::string::npos);
  }

  REQUIRE(config.Set("launchers.office.apps", nlohmann::json::array({"excel", "word"})));
  const std::string some = launchers::office::Configuration(config);
  CHECK(some.find("<ExcludeApp ID=\"Excel\"/>") == std::string::npos);
  CHECK(some.find("<ExcludeApp ID=\"Word\"/>") == std::string::npos);
  for (const char* app : {"PowerPoint", "Outlook", "OneNote"}) {
    CHECK(some.find(std::string("<ExcludeApp ID=\"") + app + "\"/>") != std::string::npos);
  }

  CHECK_FALSE(config.Set("launchers.office.apps", nlohmann::json::array()));
  CHECK_FALSE(config.Set("launchers.office.apps", nlohmann::json::array({"excel", "access"})));
}

TEST_CASE("launchers: setup output drops the lines that only look like trouble, and keeps real ones") {
  const std::string noisy =
      "Proton: Error: unable to use parent for game drive, path /home\n"
      "Running office-setup.exe\n"
      "Unhandled Exception:\n"
      "System.IO.FileNotFoundException: Could not load file or assembly 'Windows, Version=255.255.255.255, Culture=neutral'\n"
      "File name: 'Windows, Version=255.255.255.255, Culture=neutral, PublicKeyToken=null'\n"
      "  at Microsoft.Office.C2R.InspectorOfficeGadget.Main (System.String[] args) [0x00133]\n"
      "[ERROR] FATAL UNHANDLED EXCEPTION: System.IO.FileNotFoundException: 'Windows, Version=255.255.255.255\n"
      "Unhandled Exception:\n"
      "System.TypeLoadException: Could not load type of field 'xpdAgent.Log:telemetryService' (5) due to: x\n"
      "Unhandled Exception:\n"
      "System.NullReferenceException: something that is wrong\n"
      "done\n";
  CHECK(launchers::WithoutNoise(noisy) ==
        "Running office-setup.exe\n"
        "Unhandled Exception:\n"
        "System.NullReferenceException: something that is wrong\n"
        "done\n");
  CHECK(launchers::WithoutNoise("") == "");
}
