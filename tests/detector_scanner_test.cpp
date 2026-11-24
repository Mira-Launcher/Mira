#include <doctest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "api/EventBus.h"
#include "config/Config.h"
#include "config/Schema.h"
#include "core/Strings.h"
#include "library/AutoSetup.h"
#include "library/Detector.h"
#include "library/GameSetup.h"
#include "library/Scanner.h"
#include "store/GameStore.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
using test::Touch;
namespace fs = std::filesystem;

namespace {

library::DetectorSettings DefaultSettings() {
  library::DetectorSettings settings;
  settings.deny_name_patterns = {"unins*", "vcredist*", "*crashreport*"};
  settings.ignore_globs = {".*", "*/Redist*"};
  return settings;
}

}  // namespace

TEST_CASE("Detector picks the exe matching the folder name over an installer") {
  const fs::path dir = TempDir("celeste-detect");
  Touch(dir / "Celeste.exe");
  Touch(dir / "unins000.exe");

  const library::Detector detector(DefaultSettings());
  auto result = detector.Detect(dir);

  REQUIRE(result.candidates.size() == 2);
  CHECK(result.candidates[0].rel_path == "Celeste.exe");
  CHECK(result.candidates[0].chosen);
}

TEST_CASE("Detector prefers the shallower, name-matching exe in a nested folder") {
  const fs::path dir = TempDir("hollow-knight-detect");
  Touch(dir / "Content" / "unrelated_tool.exe");
  Touch(dir / "hollow_knight.exe");

  const library::Detector detector(DefaultSettings());
  auto result = detector.Detect(dir);

  REQUIRE_FALSE(result.candidates.empty());
  CHECK(result.candidates[0].rel_path == "hollow_knight.exe");

  // A store's folder named by its id: the game's own name is matched instead.
  const fs::path store_dir = TempDir("game-543813");
  Touch(store_dir / "Launcher.exe");
  Touch(store_dir / "hollow_knight.exe");
  CHECK(detector.Detect(store_dir, "Hollow Knight").candidates.front().rel_path ==
        "hollow_knight.exe");
}

TEST_CASE("Detector finds a native ELF binary, not an image an archive marked executable") {
  const fs::path dir = TempDir("celeste-native-detect");
  Touch(dir / "Celeste",
        "\x7f"
        "ELF",
        /*executable=*/true);
  Touch(dir / "Default.png", "\x89PNG", /*executable=*/true);
  Touch(dir / "Redist" / "somelib.so");  // matches the */Redist* ignore glob

  const library::Detector detector(DefaultSettings());
  auto result = detector.Detect(dir);

  REQUIRE(result.candidates.size() == 1);
  CHECK(result.candidates[0].rel_path == "Celeste");
  CHECK(result.candidates[0].kind == model::Platform::Native);
}

TEST_CASE("Detector returns no candidates for an empty folder") {
  const fs::path dir = TempDir("empty-detect");
  const library::Detector detector(DefaultSettings());
  auto result = detector.Detect(dir);
  CHECK(result.candidates.empty());
}

TEST_CASE("AutoSetup stores a native game as ready and a windows game as setting_up") {
  test::TestEnv env("autosetup-config");
  library::AutoSetup auto_setup(env.config, env.games, env.events);

  library::Detector::Result native_result;
  native_result.candidates.push_back({"Celeste", model::Platform::Native, 4.5, true});
  const fs::path root = test::TempDir("autosetup-games");
  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({root.string()})).has_value());
  model::Game native_game = auto_setup.CreateGame(root / "Celeste", native_result);
  CHECK(model::WaitsFor(native_game, model::SetupStep::Confirm));
  CHECK(native_game.id == "celeste");
  // The library folder's name doesn't become a tag.
  CHECK(native_game.tags.empty());

  library::Detector::Result windows_result;
  windows_result.candidates.push_back({"hollow_knight.exe", model::Platform::Windows, 4.5, true});
  model::Game windows_game = auto_setup.CreateGame("/games/Hollow Knight", windows_result);
  CHECK(model::WaitsFor(windows_game, model::SetupStep::Confirm));
  CHECK_FALSE(windows_game.data_dir.empty());
  // Found outside every library root (an install inside a prefix): no folder to name it after.
  CHECK(windows_game.tags.empty());

  // Answered for before it was found: nothing to ask.
  library::ConfirmWhenFound(root / "Hades",
                            {.name = "Hades II", .exe_path = "Celeste", .app = true});
  test::Touch(root / "Hades" / "Celeste", "", /*executable=*/true);
  const model::Game answered = auto_setup.CreateGame(root / "Hades", native_result);
  CHECK(answered.status == model::GameStatus::Ready);
  CHECK(answered.name == "Hades II");
  CHECK(answered.reviewed);
  CHECK(std::ranges::contains(answered.tags, "app"));
}

TEST_CASE("Scanner adds new games, skips known ones, and marks missing folders") {
  // A native game: this is about scanning, and a Windows one would pay for
  // provisioning a real prefix.
  const fs::path lib = TempDir("scan-library");
  Touch(lib / "Celeste" / "Celeste", "", /*executable=*/true);

  test::TestEnv env("scan-state");
  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({lib.string()})).has_value());
  REQUIRE(env.config.Set("prefix_root", (lib / "prefix").string()).has_value());
  library::Scanner scanner(env.config, env.games, env.events);

  // A prefix directory sitting inside the library root must never be
  // rediscovered as a game. This is the regression the design specifically
  // guards against (see docs/backend.md).
  fs::create_directories(lib / "prefix" / "celeste" / "drive_c");
  Touch(lib / "prefix" / "celeste" / "system.reg");
  // Nor is an uninstalled game's leftover tree of empty folders.
  fs::create_directories(lib / "Uninstalled" / "Data" / "Saves");

  library::ScanSummary first = scanner.ScanAll();
  CHECK(first.added == 1);
  CHECK(env.games.All().size() == 1);
  CHECK(env.games.Find("celeste").has_value());

  library::ScanSummary second = scanner.ScanAll();
  CHECK(second.added == 0);  // already known; must not be re-detected
  CHECK(env.games.All().size() == 1);

  fs::remove_all(lib / "Celeste");
  library::ScanSummary third = scanner.ScanAll();
  CHECK(third.missing == 1);
  CHECK(env.games.Find("celeste")->status == model::GameStatus::Missing);

  Touch(lib / "Celeste" / "Celeste", "", /*executable=*/true);
  library::ScanSummary fourth = scanner.ScanAll();
  CHECK(fourth.restored == 1);
  CHECK(model::WaitsFor(*env.games.Find("celeste"), model::SetupStep::Confirm));
}

TEST_CASE(
    "Scanner keeps the games of a library folder it can't read, even with remove_missing on") {
  const fs::path lib = TempDir("scan-unreadable");
  Touch(lib / "Celeste" / "Celeste", "", /*executable=*/true);

  test::TestEnv env("scan-unreadable-state");
  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({lib.string()})).has_value());
  REQUIRE(env.config.Set("library.remove_missing", true).has_value());
  library::Scanner scanner(env.config, env.games, env.events);
  REQUIRE(scanner.ScanAll().added == 1);

  fs::permissions(lib, fs::perms::owner_all, fs::perm_options::remove);
  // Root reads it regardless, as in an `unshare -r` run.
  const bool locked = ::access(lib.c_str(), R_OK) != 0;
  const library::ScanSummary unreadable = scanner.ScanAll();
  fs::permissions(lib, fs::perms::owner_all, fs::perm_options::add);
  if (!test::Has(locked, "way to lock a folder (it runs as root)")) return;
  CHECK(unreadable.missing == 0);
  REQUIRE(env.games.Find("celeste").has_value());
  CHECK(model::WaitsFor(*env.games.Find("celeste"), model::SetupStep::Confirm));
}

TEST_CASE("Scanner finds an AppImage loose in a library folder as a game of its own") {
  const fs::path lib = TempDir("scan-appimage-library");
  Touch(lib / "osu.AppImage", "appimage", /*executable=*/true);
  Touch(lib / "notes.txt", "not a game");

  test::TestEnv env("scan-appimage-state");
  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({lib.string()})).has_value());
  library::Scanner scanner(env.config, env.games, env.events);

  CHECK(scanner.ScanAll().added == 1);
  const auto osu = env.games.Find("osu");
  REQUIRE(osu.has_value());
  CHECK(osu->exe_path == "osu.AppImage");
  CHECK(model::WaitsFor(*osu, model::SetupStep::Confirm));
  CHECK(scanner.ScanAll().added == 0);

  fs::remove(lib / "osu.AppImage");
  CHECK(scanner.ScanAll().missing == 1);
  CHECK(env.games.Find("osu")->status == model::GameStatus::Missing);
  Touch(lib / "osu.AppImage", "appimage", /*executable=*/true);
  CHECK(scanner.ScanAll().restored == 1);

  // One added by hand from a subfolder, recorded against the library folder, is still there.
  Touch(lib / "emu" / "Eden.AppImage", "appimage", /*executable=*/true);
  model::Game eden;
  eden.id = "eden";
  eden.install_path = lib.string();
  eden.exe_path = "emu/Eden.AppImage";
  eden.status = model::GameStatus::Ready;
  REQUIRE(env.games.Upsert(eden).has_value());
  scanner.ScanAll();
  CHECK(env.games.Find("eden")->status == model::GameStatus::Ready);
}

TEST_CASE("Detector never descends into a nested wine prefix during its own walk") {
  // Regression: a wrapper folder whose actual prefix sits one level below
  // itself (umu's own layout, e.g. <root>/umu/umu-default/) must not have
  // its prefix's internal .exe files picked up as game candidates. Found by
  // testing against a real library, not by review.
  const fs::path dir = TempDir("nested-prefix-detect");
  fs::create_directories(dir / "umu-default" / "drive_c" / "windows" / "system32");
  Touch(dir / "umu-default" / "system.reg");
  Touch(dir / "umu-default" / "drive_c" / "windows" / "system32" / "notepad.exe");

  const library::Detector detector(DefaultSettings());
  auto result = detector.Detect(dir);
  CHECK(result.candidates.empty());
}

TEST_CASE(
    "A game kept in its own prefix lists its own executables, not Wine's or what the prefix's "
    "links lead to") {
  // Wine links dosdevices/z: to / and the user folders to the real home.
  const fs::path outside = TempDir("prefix-links-outside");
  Touch(outside / "bin" / "head", "elf", /*executable=*/true);
  Touch(outside / "Documents" / "Tool.exe");
  const fs::path prefix = TempDir("prefix-links-game");
  Touch(prefix / "system.reg");
  Touch(prefix / "Cuphead.exe");
  Touch(prefix / "drive_c" / "windows" / "system32" / "notepad.exe");
  fs::create_directories(prefix / "dosdevices");
  fs::create_directory_symlink(outside, prefix / "dosdevices" / "z:");
  fs::create_directories(prefix / "drive_c" / "users" / "me");
  fs::create_directory_symlink(outside / "Documents",
                               prefix / "drive_c" / "users" / "me" / "Documents");

  const auto rel_paths = [](const std::vector<model::Candidate>& candidates) {
    std::vector<std::string> paths;
    for (const model::Candidate& candidate : candidates) paths.push_back(candidate.rel_path);
    return paths;
  };
  const library::Detector detector(DefaultSettings());
  CHECK(rel_paths(detector.Detect(prefix).candidates) == std::vector<std::string>{"Cuphead.exe"});
}

TEST_CASE("A scan never provisions a game nobody has confirmed") {
  const fs::path lib = TempDir("scan-no-autosetup-library");
  fs::create_directories(lib / "Celeste");
  Touch(lib / "Celeste" / "Celeste.exe");

  test::TestEnv env("scan-no-autosetup-state");
  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({lib.string()})).has_value());
  REQUIRE(env.config.Set("prefix_root", (lib / "prefix").string()).has_value());
  library::Scanner scanner(env.config, env.games, env.events);

  library::ScanSummary summary = scanner.ScanAll();
  CHECK(summary.added == 1);

  auto celeste = env.games.Find("celeste");
  REQUIRE(celeste.has_value());
  CHECK(model::WaitsFor(*celeste, model::SetupStep::Confirm));
  CHECK(celeste->runner_ref.empty());
  CHECK_FALSE(fs::exists(celeste->data_dir));
}

TEST_CASE("Scanner leaves alone the folder of an installer whose game now lives elsewhere") {
  const fs::path lib = TempDir("scan-installer-dir-library");
  Touch(lib / "Setup Clustertruck" / "setup_clustertruck.exe");
  test::TestEnv env("scan-installer-dir-state");
  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({lib.string()})).has_value());
  REQUIRE(env.config.Set("auto_setup", false).has_value());

  model::Game installed;
  installed.id = "clustertruck";
  installed.name = "ClusterTruck";
  installed.install_path =
      (env.dir / "prefixes" / "clustertruck" / "drive_c" / "ClusterTruck").string();
  installed.installer_dir = (lib / "Setup Clustertruck").string();
  REQUIRE(env.games.Upsert(installed).has_value());

  CHECK(library::Scanner(env.config, env.games, env.events).ScanAll().added == 0);
  CHECK(env.games.All().size() == 1);

  // Nor the folder of a game that runs from a subfolder of it.
  Touch(lib / "Batman" / "Binaries" / "BmLauncher.exe");
  model::Game nested;
  nested.id = "batman";
  nested.name = "Batman";
  nested.install_path = (lib / "Batman" / "Binaries").string();
  REQUIRE(env.games.Upsert(nested).has_value());
  CHECK(library::Scanner(env.config, env.games, env.events).ScanAll().added == 0);
  CHECK(env.games.All().size() == 2);
}

TEST_CASE("Scanner retries provisioning for games left setting_up or failed by a missing runner") {
  const fs::path lib = TempDir("scan-retry-library");
  Touch(lib / "Celeste" / "Celeste.exe");

  test::TestEnv env("scan-retry-state");
  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({lib.string()})).has_value());
  REQUIRE(env.config.Set("auto_setup", false).has_value());
  library::Scanner scanner(env.config, env.games, env.events);

  // First scan: the game waits to be confirmed; confirmed, it waits for its runner.
  scanner.ScanAll();
  auto celeste = env.games.Find("celeste");
  REQUIRE(celeste.has_value());
  CHECK(model::WaitsFor(*celeste, model::SetupStep::Confirm));
  REQUIRE(library::ConfirmSetup(env.config, env.games, *celeste, {}));
  REQUIRE(env.games.Upsert(*celeste));
  CHECK(env.games.Find("celeste")->status == model::GameStatus::SettingUp);

  // auto_setup turns on later, a later scan of the same, already-known
  // folder must retry rather than skip it forever.
  // With no runner installed it breaks, and comes back once one resolves.
  REQUIRE(env.config.Set("auto_setup", true).has_value());
  REQUIRE(env.config.Set("default_runner.windows", "wine:missing").has_value());
  scanner.ScanAll();
  celeste = env.games.Find("celeste");
  REQUIRE(celeste.has_value());
  CHECK(model::WaitsFor(*celeste, model::SetupStep::Failed));

  REQUIRE(env.config.Set("default_runner.windows", "native:native").has_value());
  scanner.ScanAll();
  CHECK(env.games.Find("celeste")->status == model::GameStatus::Ready);
}

TEST_CASE("An installer is told by its name alone and ranks between the game and its helpers") {
  const auto detect = [](const fs::path& dir) {
    library::DetectorSettings settings = DefaultSettings();
    settings.installer_name_patterns = {"setup*", "*setup*", "install*", "*installer*"};
    settings.deny_name_patterns = {"unins*", "setup*", "*redist*", "quicksfv*"};
    return library::Detector(settings).Detect(dir).candidates;
  };
  const fs::path repack = TempDir("repack-detect");
  Touch(repack / "setup.exe");
  fs::create_directories(repack / "MD5");
  Touch(repack / "MD5" / "QuickSFV.EXE");
  const auto packed = detect(repack);
  REQUIRE(packed.size() == 2);
  CHECK(packed[0].rel_path == "setup.exe");
  CHECK(packed[0].is_installer);
  CHECK_FALSE(packed[1].is_installer);

  const fs::path installed = TempDir("installed-detect");
  Touch(installed / "Genesis.exe");
  Touch(installed / "setup.exe");
  Touch(installed / "unins000.exe");
  const auto game = detect(installed);
  REQUIRE(game.size() == 3);
  CHECK(game[0].rel_path == "Genesis.exe");
  CHECK(game[1].is_installer);
  CHECK_FALSE(game[2].is_installer);
}

TEST_CASE("Nothing in a redistributables, hidden or underscored folder is a candidate") {
  const fs::path dir = TempDir("never-game-detect");
  for (const char* folder : {"_CommonRedist", "Redist", ".cache", "__Installer", "bin"}) {
    fs::create_directories(dir / folder);
    Touch(dir / folder / "game.exe");
  }
  const auto candidates = library::Detector(DefaultSettings()).Detect(dir).candidates;
  REQUIRE(candidates.size() == 1);
  CHECK(candidates[0].rel_path == "bin/game.exe");
}

TEST_CASE("the real default deny list excludes known engine/launcher helper executables") {
  // Regression for a documented, still-open Lutris bug (lutris/lutris#6881):
  // UnityCrashHandler64.exe getting auto-picked over the real game exe.
  // Uses the actual shipped schema default, not the test's minimal one
  // above, so this fails if that default ever regresses.
  library::DetectorSettings settings;
  settings.deny_name_patterns = config::Schema::Instance()
                                    .Find("detect.deny_name_patterns")
                                    ->default_value.get<std::vector<std::string>>();

  const fs::path dir = TempDir("engine-helpers-detect");
  Touch(dir / "SomeGame.exe");
  Touch(dir / "UnityCrashHandler64.exe");
  Touch(dir / "UnrealCEFSubProcess.exe");
  Touch(dir / "EpicWebHelper.exe");
  Touch(dir / "UplayCrashReporter.exe");

  const library::Detector detector(settings);
  auto result = detector.Detect(dir);

  REQUIRE_FALSE(result.candidates.empty());
  CHECK(result.candidates[0].rel_path == "SomeGame.exe");
}

TEST_CASE("a restored game keeps waiting for its installer instead of becoming launchable") {
  // Regression: the Missing->restore path keyed on "has an exe_path", but an
  // installer has one too (kept for reference), so unplugging and replugging
  // a drive silently promoted it to ready, pointed straight at
  // setup.exe, undoing the installer guard entirely.
  const fs::path lib = TempDir("restore-installer-library");
  fs::create_directories(lib / "game-hollow");
  const fs::path installer = lib / "game-hollow" / "setup_hollow_knight.exe";
  {
    std::ofstream out(installer, std::ios::binary);
    out.seekp(60 * 1024 * 1024 - 1);
    out.put('\0');
  }

  test::TestEnv env("restore-installer-state");
  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({lib.string()})).has_value());
  REQUIRE(env.config.Set("prefix_root", (lib / "prefixes").string()).has_value());
  library::Scanner scanner(env.config, env.games, env.events);

  REQUIRE(scanner.ScanAll().added == 1);
  REQUIRE(model::WaitsFor(*env.games.Find("game-hollow"), model::SetupStep::Install));

  // Out of the library, since a folder moved within it is followed.
  const fs::path away = TempDir("restore-installer-away") / "game-hollow";
  fs::rename(lib / "game-hollow", away);  // "drive unplugged"
  REQUIRE(scanner.ScanAll().missing == 1);
  REQUIRE(env.games.Find("game-hollow")->status == model::GameStatus::Missing);

  fs::rename(away, lib / "game-hollow");  // and back
  scanner.ScanAll();
  CHECK(model::WaitsFor(*env.games.Find("game-hollow"), model::SetupStep::Install));
}

TEST_CASE("a restored windows game with no prefix is not claimed ready") {
  const fs::path lib = TempDir("restore-unprovisioned-library");
  Touch(lib / "Celeste" / "Celeste.exe");

  test::TestEnv env("restore-unprovisioned-state");
  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({lib.string()})).has_value());
  REQUIRE(env.config.Set("prefix_root", (lib / "prefixes").string()).has_value());
  REQUIRE(env.config.Set("auto_setup", false).has_value());  // never provisioned
  library::Scanner scanner(env.config, env.games, env.events);

  REQUIRE(scanner.ScanAll().added == 1);
  model::Game celeste = *env.games.Find("celeste");
  REQUIRE(library::ConfirmSetup(env.config, env.games, celeste, {}));
  REQUIRE(env.games.Upsert(celeste));

  const fs::path away = TempDir("restore-unprovisioned-away") / "Celeste";
  fs::rename(lib / "Celeste", away);
  REQUIRE(scanner.ScanAll().missing == 1);
  fs::rename(away, lib / "Celeste");
  scanner.ScanAll();

  // No runner_ref, no prefix on disk: "ready" would be a lie.
  CHECK(env.games.Find("celeste")->status == model::GameStatus::SettingUp);
}

TEST_CASE("Detector picks the game over what ships with it") {
  struct Layout {
    const char* folder;
    const char* pick;
    std::vector<const char*> files;
  };
  const std::vector<Layout> layouts = {
      {"Bloons-TD-6",
       "BloonsTD6/BloonsTD6.exe",
       {"BloonsTD6/BloonsTD6_Data/globalgamemanagers", "BloonsTD6/UnityCrashHandler64.exe",
        "BloonsTD6/Cleaner/Cleaner-BTD6.exe", "BloonsTD6/BloonsTD6.exe"}},
      {"SUPERHOT",
       "SUPERHOT.exe",
       {"SH.exe", "SH_Data/FFMPEGEncoding/ffmpeg.exe", "SUPERHOT.exe", "SUPERHOT_Data/level0"}},
      {"game-280042",
       "SUPERHOT/SUPERHOT.exe",
       {"SUPERHOT/SH.exe", "SUPERHOT/SH_Data/level0", "SUPERHOT/SUPERHOT.exe",
        "SUPERHOT/SUPERHOT_Data/level0"}},
      {"assassin-s-creed-2",
       "AssassinsCreedIIGame.exe",
       {"UPlayBrowser.exe", "UplayInstaller.exe", "AssassinsCreedIIGame.exe"}},
      // Nothing like the game's name: the engine's files tell.
      {"Nine Nights", "nn.exe", {"Config.exe", "nn.exe", "nn.pck", "Nine Nights Server.exe"}},
      {"Deep Rock Galactic",
       "FSD.exe",
       {"FSD.exe", "FSD/Binaries/Win64/FSD-Win64-Shipping.exe", "Engine/Extras/CrashReport.exe",
        "Engine/Binaries/Win64/EpicWebHelper.exe"}},
      {"Stray Cats",
       "Hk/Binaries/Win64/Hk-Win64-Shipping.exe",
       {"Hk/Binaries/Win64/Hk-Win64-Shipping.exe", "Engine/Binaries/Win64/UnrealCEF.exe"}},
      {"OurRedString-v15.2.3-pc",
       "OurRedString.sh",
       {"OurRedString.exe", "OurRedString.sh", "OurRedString.py", "renpy/common.rpy",
        "lib/py3-windows-x86_64/pythonw.exe", "lib/py3-windows-x86_64/OurRedString.exe"}},
      {"Dune Runner",
       "DuneRunner.sh",
       {"DuneRunner.exe", "DuneRunner.sh", "DuneRunner.bin.x86_64", "FNA.dll"}},
      {"The Evil Within 2", "TEW2.exe", {"TEW2.exe", "Launcher.exe", "redist_tool.exe"}},
      {"Server Simulator", "Server Simulator.exe", {"Server Simulator.exe", "Editor.exe"}},
      {"Old Game", "bin/x64/game.exe", {"bin/x86/game.exe", "bin/x64/game.exe"}},
  };
  const test::TestEnv env("detector-layouts");
  const library::Detector detector(library::SettingsFromConfig(env.config));
  int at = 0;
  for (const Layout& layout : layouts) {
    const fs::path dir = TempDir("detector-layout-" + std::to_string(at++)) / layout.folder;
    std::vector<library::Detector::ListedFile> listed;
    for (const char* file : layout.files) {
      Touch(dir / file);
      listed.push_back({file});
    }
    CAPTURE(layout.folder);
    CHECK(detector.Detect(dir).candidates.front().rel_path == layout.pick);
    // As told from an archive's listing.
    CHECK(detector.DetectListed(listed, layout.folder).candidates.front().rel_path == layout.pick);
  }
}
