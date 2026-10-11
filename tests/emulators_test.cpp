#include <algorithm>
#include <doctest.h>

#include <filesystem>

#include "core/Strings.h"
#include "emulators/Emulators.h"
#include "library/Scanner.h"
#include "runner/EmulatorRunner.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

TEST_CASE("emulators: ROM names lose their dump tags but keep disc numbers") {
  CHECK(emulators::CleanName("Metroid Fusion (USA) [!].gba") == "Metroid Fusion");
  CHECK(emulators::CleanName("Final_Fantasy_VII (USA) (Disc 2).cue") == "Final Fantasy VII (Disc 2)");
  CHECK(emulators::CleanName("Chrono Trigger (Rev 1).sfc") == "Chrono Trigger");
}

TEST_CASE("emulators: a folder imports its games, hides what a cue names, and they go missing") {
  test::TestEnv env("emulators-import");
  const emulators::Emulator& duckstation = *emulators::Find("duckstation");
  const emulators::Emulator& rpcs3 = *emulators::Find("rpcs3");
  const fs::path ps1 = emulators::RomDir(env.config, duckstation);
  test::Touch(ps1 / "Crash" / "Crash Bandicoot (USA).cue", "FILE \"Crash Bandicoot (USA).bin\" BINARY\n");
  test::Touch(ps1 / "Crash" / "Crash Bandicoot (USA).bin");
  test::Touch(ps1 / "Spyro (USA).chd");
  test::Touch(ps1 / "notes.txt");
  test::Touch(ps1 / "Homebrew" / "EBOOT.PBP");
  test::Touch(ps1 / "Homebrew" / "module.pbp");
  test::Touch(emulators::RomDir(env.config, rpcs3) / "BLUS30001" / "PS3_GAME" / "USRDIR" / "EBOOT.BIN");

  const auto summary = emulators::Import(env.config, env.games, env.events, duckstation);
  REQUIRE(summary);
  CHECK(summary->added == 3);
  const auto crash = env.games.Find("duckstation-crash-crash-bandicoot-usa");
  REQUIRE(crash);
  CHECK(crash->name == "Crash Bandicoot");
  CHECK(crash->exe_path == "Crash Bandicoot (USA).cue");
  CHECK(crash->runner_ref == "emulator:duckstation");
  CHECK(emulators::GameFiles(env.config, *crash).size() == 2);
  CHECK(env.games.Find("duckstation-homebrew-eboot")->name == "Homebrew");

  REQUIRE(emulators::Import(env.config, env.games, env.events, rpcs3));
  const auto ps3 = env.games.Find("rpcs3-blus30001");
  REQUIRE(ps3);
  CHECK(ps3->exe_path == "PS3_GAME/USRDIR/EBOOT.BIN");

  // A DOS game is its folder, started by the file named like it; one the user picks stays picked.
  const emulators::Emulator& dosbox = *emulators::Find("dosbox");
  const fs::path dos = emulators::RomDir(env.config, dosbox);
  test::Touch(dos / "Doom" / "SETUP.EXE");
  test::Touch(dos / "Doom" / "DOOM.EXE");
  test::Touch(dos / "Keen" / "INSTALL.BAT");
  test::Touch(dos / "Keen" / "KEEN4E.EXE");
  REQUIRE(emulators::Import(env.config, env.games, env.events, dosbox));
  CHECK(env.games.Find("dosbox-doom")->exe_path == "DOOM.EXE");
  CHECK(env.games.Find("dosbox-keen")->exe_path == "KEEN4E.EXE");
  CHECK(emulators::StartChoices(env.config, *env.games.Find("dosbox-keen")) ==
        std::vector<std::string>{"KEEN4E.EXE", "INSTALL.BAT"});
  CHECK(emulators::StartChoices(env.config, *env.games.Find("dosbox-doom")).empty());
  REQUIRE(env.games.Update("dosbox-doom", [](model::Game& g) { g.exe_path = "SETUP.EXE"; }));
  REQUIRE(emulators::Import(env.config, env.games, env.events, dosbox));
  CHECK(env.games.Find("dosbox-doom")->exe_path == "SETUP.EXE");
  CHECK(env.games.All().size() == 6);

  fs::remove(ps1 / "Spyro (USA).chd");
  REQUIRE(emulators::Import(env.config, env.games, env.events, duckstation));
  CHECK(env.games.Find("duckstation-spyro-usa")->status == model::GameStatus::Missing);
}

TEST_CASE("emulators: added files are copied or moved in, run, and move to another emulator") {
  test::TestEnv env("emulators-add");
  const emulators::Emulator& snes9x = *emulators::Find("snes9x");
  const emulators::Emulator& mesen = *emulators::Find("mesen");
  test::Touch(env.dir / "bin" / "snes9x", "", true);
  test::Touch(env.dir / "bin" / "Mesen", "", true);
  REQUIRE(env.config.Set("emulators.snes9x.bin", (env.dir / "bin" / "snes9x").string()));
  REQUIRE(env.config.Set("emulators.mesen.bin", (env.dir / "bin" / "Mesen").string()));
  const fs::path downloads = env.dir / "Downloads";
  test::Touch(downloads / "Super Metroid (USA).sfc");
  test::Touch(downloads / "Earthbound (USA).smc");
  test::Touch(downloads / "readme.txt");

  CHECK_FALSE(emulators::AddFiles(env.config, env.games, env.events, snes9x, {downloads / "readme.txt"}, false));
  test::Touch(downloads / "CMFileManager" / "EBOOT.PBP");
  REQUIRE(emulators::AddFiles(env.config, env.games, env.events, *emulators::Find("ppsspp"),
                              {downloads / "CMFileManager" / "EBOOT.PBP"}, false));
  CHECK(env.games.Find("ppsspp-cmfilemanager-eboot")->name == "CMFileManager");
  REQUIRE(emulators::AddFiles(env.config, env.games, env.events, snes9x, {downloads / "Super Metroid (USA).sfc"}, false));
  REQUIRE(emulators::AddFiles(env.config, env.games, env.events, snes9x, {downloads / "Earthbound (USA).smc"}, true));
  CHECK(fs::exists(downloads / "Super Metroid (USA).sfc"));
  CHECK_FALSE(fs::exists(downloads / "Earthbound (USA).smc"));

  auto game = env.games.Find("snes9x-super-metroid-usa");
  REQUIRE(game);
  const runner::EmulatorRunner runner(env.config);
  const auto command = runner.BuildCommand(*game, std::nullopt);
  REQUIRE(command);
  CHECK(command->argv == std::vector<std::string>{(env.dir / "bin" / "snes9x").string(),
                                                  (emulators::RomDir(env.config, snes9x) / "Super Metroid (USA).sfc").string()});

  // Others on this computer's PATH (ares, bsnes) may be offered too.
  CHECK(std::ranges::contains(emulators::Alternatives(env.config, *game), &mesen));
  const auto moved = emulators::MoveGame(env.config, env.games, *game, mesen);
  REQUIRE(moved);
  CHECK(moved->source == "mesen");
  CHECK(fs::exists(emulators::RomDir(env.config, mesen) / "Super Metroid (USA).sfc"));
  // Mesen's import finds it as the same game.
  const auto summary = emulators::Import(env.config, env.games, env.events, mesen);
  REQUIRE(summary);
  CHECK(summary->added == 0);
}

TEST_CASE("emulators: download sources read as a repository or link") {
  CHECK(emulators::DefaultSource(*emulators::Find("duckstation")) == "stenzek/duckstation");
  CHECK(emulators::DefaultSource(*emulators::Find("bsnes")) == "bsnes-emu/bsnes");
  CHECK(emulators::DefaultSource(*emulators::Find("ryujinx")).starts_with("https://mirror.cachyos.org/"));
  CHECK(strings::GithubRepo("https://github.com/someone/duckstation") == "someone/duckstation");
  CHECK(strings::GithubRepo("someone/duckstation") == "someone/duckstation");
}

TEST_CASE("emulators: an emulator's AppImage in a library folder sets up its source instead of a game") {
  CHECK(emulators::Recognize("DuckStation-x64.AppImage")->id == "duckstation");
  CHECK(emulators::Recognize("pcsx2-v2.2.0-linux-appimage-x64-Qt.AppImage")->id == "pcsx2");
  CHECK(emulators::Recognize("Dolphin_Emulator-x86_64.AppImage")->id == "dolphin");
  CHECK(emulators::Recognize("Eden Rising.AppImage") == nullptr);
  CHECK(emulators::Recognize("Ares-of-War-1.0.AppImage") == nullptr);
  CHECK(emulators::Recognize("Celeste.AppImage") == nullptr);

  test::TestEnv env("emulators-adopt");
  const fs::path root = env.dir / "Applications";
  test::Touch(root / "mGBA-0.10.5-appimage-x64.appimage", "", true);
  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({root.string()})));
  library::Scanner(env.config, env.games, env.events).ScanAll();
  CHECK(env.games.All().empty());
  CHECK(env.config.GetString("emulators.mgba.bin") == (root / "mGBA-0.10.5-appimage-x64.appimage").string());
  CHECK(env.games.Source("mgba").added);
}
