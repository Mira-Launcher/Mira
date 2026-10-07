#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "api/EventBus.h"
#include "config/Config.h"
#include "config/Schema.h"
#include "library/AutoSetup.h"
#include "library/Detector.h"
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
  CHECK(result.confidence > 0.5);
}

TEST_CASE("Detector prefers the shallower, name-matching exe in a nested folder") {
  const fs::path dir = TempDir("hollow-knight-detect");
  Touch(dir / "Content" / "unrelated_tool.exe");
  Touch(dir / "hollow_knight.exe");

  const library::Detector detector(DefaultSettings());
  auto result = detector.Detect(dir);

  REQUIRE_FALSE(result.candidates.empty());
  CHECK(result.candidates[0].rel_path == "hollow_knight.exe");
}

TEST_CASE("Detector finds a native ELF binary via its execute bit, ignoring redist noise") {
  const fs::path dir = TempDir("celeste-native-detect");
  Touch(dir / "Celeste", "", /*executable=*/true);
  Touch(dir / "Redist" / "somelib.so");  // matches the */Redist* ignore glob

  const library::Detector detector(DefaultSettings());
  auto result = detector.Detect(dir);

  REQUIRE(result.candidates.size() == 1);
  CHECK(result.candidates[0].rel_path == "Celeste");
  CHECK(result.candidates[0].kind == model::Platform::Native);
}

TEST_CASE("Detector returns no candidates and low confidence for an empty folder") {
  const fs::path dir = TempDir("empty-detect");
  const library::Detector detector(DefaultSettings());
  auto result = detector.Detect(dir);
  CHECK(result.candidates.empty());
  CHECK(result.confidence == doctest::Approx(0.0));
}

TEST_CASE("AutoSetup stores a native game as ready and a windows game as setting_up") {
  test::TestEnv env("autosetup-config");
  library::AutoSetup auto_setup(env.config, env.games, env.events);

  library::Detector::Result native_result;
  native_result.candidates.push_back({"Celeste", model::Platform::Native, 4.5, true});
  native_result.confidence = 0.9;
  model::Game native_game = auto_setup.CreateGame("/games/Celeste", native_result);
  CHECK(native_game.status == model::GameStatus::Ready);
  CHECK(native_game.id == "celeste");
  // scan.tag_by_root defaults on -- the parent folder's own name ("games")
  // becomes a tag automatically, with no config needed.
  REQUIRE(native_game.tags.size() == 1);
  CHECK(native_game.tags[0] == "games");

  library::Detector::Result windows_result;
  windows_result.candidates.push_back(
      {"hollow_knight.exe", model::Platform::Windows, 4.5, true});
  windows_result.confidence = 0.8;
  model::Game windows_game =
      auto_setup.CreateGame("/games/Hollow Knight", windows_result);
  CHECK(windows_game.status == model::GameStatus::SettingUp);
  CHECK_FALSE(windows_game.data_dir.empty());

  auto stream = env.events.Since(0);
  REQUIRE(stream.size() == 2);
  CHECK(stream[0].type == "game.added");
  CHECK(stream[0].payload.value("open_config", false) == true);
}

TEST_CASE("AutoSetup skips the automatic root tag when scan.tag_by_root is off") {
  test::TestEnv env("autosetup-no-tag-config");
  REQUIRE(env.config.Set("scan.tag_by_root", false).has_value());
  library::AutoSetup auto_setup(env.config, env.games, env.events);

  library::Detector::Result result;
  result.candidates.push_back({"Celeste", model::Platform::Native, 4.5, true});
  result.confidence = 0.9;
  model::Game game = auto_setup.CreateGame("/games/Celeste", result);
  CHECK(game.tags.empty());
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
  // guards against (see docs/architecture.md).
  fs::create_directories(lib / "prefix" / "celeste" / "drive_c");
  Touch(lib / "prefix" / "celeste" / "system.reg");

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
  CHECK(env.games.Find("celeste")->status == model::GameStatus::Ready);
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
  const library::ScanSummary unreadable = scanner.ScanAll();
  fs::permissions(lib, fs::perms::owner_all, fs::perm_options::add);
  CHECK(unreadable.missing == 0);
  REQUIRE(env.games.Find("celeste").has_value());
  CHECK(env.games.Find("celeste")->status == model::GameStatus::Ready);
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
  CHECK(osu->status == model::GameStatus::Ready);
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

TEST_CASE("A game kept in its own prefix lists its own executables, not Wine's or what the prefix's links lead to") {
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
  fs::create_directory_symlink(outside / "Documents", prefix / "drive_c" / "users" / "me" / "Documents");

  const auto rel_paths = [](const std::vector<model::Candidate>& candidates) {
    std::vector<std::string> paths;
    for (const model::Candidate& candidate : candidates) paths.push_back(candidate.rel_path);
    return paths;
  };
  const library::Detector detector(DefaultSettings());
  CHECK(rel_paths(detector.Detect(prefix).candidates) == std::vector<std::string>{"Cuphead.exe"});
}

TEST_CASE("Scanner does not auto-provision when auto_setup is off") {
  const fs::path lib = TempDir("scan-no-autosetup-library");
  fs::create_directories(lib / "Celeste");
  Touch(lib / "Celeste" / "Celeste.exe");

  test::TestEnv env("scan-no-autosetup-state");
  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({lib.string()})).has_value());
  REQUIRE(env.config.Set("prefix_root", (lib / "prefix").string()).has_value());
  REQUIRE(env.config.Set("auto_setup", false).has_value());
  library::Scanner scanner(env.config, env.games, env.events);

  library::ScanSummary summary = scanner.ScanAll();
  CHECK(summary.added == 1);

  auto celeste = env.games.Find("celeste");
  REQUIRE(celeste.has_value());
  // Detected and stored (the frontend can still see and configure it), but
  // never auto-provisioned: still setting_up, no runner_ref pinned, no
  // prefix created.
  CHECK(celeste->status == model::GameStatus::SettingUp);
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

TEST_CASE("Scanner retries provisioning for games left setting_up or broken by a missing runner") {
  const fs::path lib = TempDir("scan-retry-library");
  Touch(lib / "Celeste" / "Celeste.exe");

  test::TestEnv env("scan-retry-state");
  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({lib.string()})).has_value());
  REQUIRE(env.config.Set("auto_setup", false).has_value());
  library::Scanner scanner(env.config, env.games, env.events);

  // First scan: auto_setup off, so the game is detected but left setting_up.
  scanner.ScanAll();
  auto celeste = env.games.Find("celeste");
  REQUIRE(celeste.has_value());
  CHECK(celeste->status == model::GameStatus::SettingUp);

  // auto_setup turns on later, a later scan of the same, already-known
  // folder must retry rather than skip it forever.
  // With no runner installed it breaks, and comes back once one resolves.
  REQUIRE(env.config.Set("auto_setup", true).has_value());
  REQUIRE(env.config.Set("default_runner.windows", "wine:missing").has_value());
  scanner.ScanAll();
  celeste = env.games.Find("celeste");
  REQUIRE(celeste.has_value());
  CHECK(celeste->status == model::GameStatus::Broken);

  REQUIRE(env.config.Set("default_runner.windows", "native:native").has_value());
  scanner.ScanAll();
  CHECK(env.games.Find("celeste")->status == model::GameStatus::Ready);
}

TEST_CASE("Detector flags a large setup.exe as an installer, not the game") {
  const fs::path dir = TempDir("hollow-installer-detect");
  const fs::path installer = dir / "setup_hollow_knight_1.5.12620_(64bit)_(89718).exe";
  {
    // Needs to clear the size floor for real; a truncated stub must not count.
    std::ofstream out(installer, std::ios::binary);
    out.seekp(60 * 1024 * 1024 - 1);
    out.put('\0');
  }

  library::DetectorSettings settings = DefaultSettings();
  settings.installer_name_patterns = {"setup*", "*setup*", "install*", "*installer*"};
  settings.installer_min_size_mb = 50;
  const library::Detector detector(settings);
  auto result = detector.Detect(dir);

  REQUIRE(result.candidates.size() == 1);
  CHECK(result.candidates[0].is_installer);
}

TEST_CASE("A name match under the size floor is not flagged as an installer") {
  const fs::path dir = TempDir("small-setup-detect");
  Touch(dir / "setup_language_pack.exe");  // name matches, but it's tiny

  library::DetectorSettings settings = DefaultSettings();
  settings.installer_name_patterns = {"setup*"};
  settings.installer_min_size_mb = 50;
  const library::Detector detector(settings);
  auto result = detector.Detect(dir);

  REQUIRE(result.candidates.size() == 1);
  CHECK_FALSE(result.candidates[0].is_installer);
}

TEST_CASE("A small installer is recognised by its builder's mark, whatever its name") {
  const fs::path dir = TempDir("signature-installer-detect");
  const auto installer_like = [](const fs::path& path, std::string_view mark) {
    std::ofstream out(path, std::ios::binary);
    out << "MZ" << std::string(300 * 1024, '\0') << "<assemblyIdentity name=\"" << mark << "\"/>";
  };
  installer_like(dir / "Moonleap-1.2-win.exe", "Inno Setup");
  installer_like(dir / "setup_tiny.exe", "Nullsoft.NSIS.exehead");
  installer_like(dir / "unins000.exe", "Inno Setup");         // its uninstaller, on the deny list
  installer_like(dir / "VC_redist.x64.exe", ".wixburn");       // a redistributable, likewise
  Touch(dir / "Moonleap.exe");

  library::DetectorSettings settings = DefaultSettings();
  settings.installer_name_patterns = {"setup*", "*setup*", "install*", "*installer*"};
  settings.deny_name_patterns = {"unins*", "*redist*"};
  settings.installer_min_size_mb = 50;
  const auto result = library::Detector(settings).Detect(dir);

  const auto flagged = [&](const std::string& name) {
    const auto it = std::ranges::find_if(result.candidates, [&](const auto& c) { return c.rel_path == name; });
    REQUIRE(it != result.candidates.end());
    return it->is_installer;
  };
  CHECK(flagged("Moonleap-1.2-win.exe"));
  CHECK(flagged("setup_tiny.exe"));
  CHECK_FALSE(flagged("unins000.exe"));
  CHECK_FALSE(flagged("VC_redist.x64.exe"));
  CHECK_FALSE(flagged("Moonleap.exe"));
}

TEST_CASE("AutoSetup stores an installer candidate as needs_install, not launchable") {
  test::TestEnv env("autosetup-installer-config");
  library::AutoSetup auto_setup(env.config, env.games, env.events);

  library::Detector::Result detected;
  detected.candidates.push_back({"setup_hollow_knight.exe", model::Platform::Windows, 4.0, true, true});
  detected.confidence = 1.0;

  model::Game game = auto_setup.CreateGame("/games/game-hollow", detected);
  CHECK(game.status == model::GameStatus::NeedsInstall);
  CHECK_FALSE(game.last_error.empty());
  CHECK(game.exe_path == "setup_hollow_knight.exe");  // kept for reference, just not launchable yet
}

TEST_CASE("A small installer stub is still flagged if a large sibling payload sits beside it") {
  // Regression: InstallShield/Inno Setup split installers are commonly a
  // few-MB launcher .exe next to a much larger separate .bin/.cab payload,
  // found against a real Wingspan install where every setup_*.exe was under
  // 7MB but sat beside a 1.8GB .bin file. Checking only the exe's own size
  // missed this entirely.
  const fs::path dir = TempDir("split-installer-detect");
  Touch(dir / "setup_wingspan_313.exe");  // a few KB; the real files are ~1-6MB
  {
    std::ofstream out(dir / "setup_wingspan_313-1.bin", std::ios::binary);
    out.seekp(60 * 1024 * 1024 - 1);
    out.put('\0');
  }

  library::DetectorSettings settings = DefaultSettings();
  settings.installer_name_patterns = {"setup*"};
  settings.installer_min_size_mb = 50;
  const library::Detector detector(settings);
  auto result = detector.Detect(dir);

  REQUIRE(result.candidates.size() == 1);
  CHECK(result.candidates[0].is_installer);
}

TEST_CASE("the real default deny list excludes known engine/launcher helper executables") {
  // Regression for a documented, still-open Lutris bug (lutris/lutris#6881):
  // UnityCrashHandler64.exe getting auto-picked over the real game exe.
  // Uses the actual shipped schema default, not the test's minimal one
  // above, so this fails if that default ever regresses.
  library::DetectorSettings settings;
  settings.deny_name_patterns = config::Schema::Instance().Find("detect.deny_name_patterns")
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

TEST_CASE("a restored game keeps needs_install instead of becoming launchable") {
  // Regression: the Missing->restore path keyed on "has an exe_path", but an
  // installer has one too (kept for reference), so unplugging and replugging
  // a drive silently promoted needs_install to ready, pointed straight at
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
  REQUIRE(env.games.Find("game-hollow")->status == model::GameStatus::NeedsInstall);

  fs::rename(lib / "game-hollow", lib / "game-hollow-away");  // "drive unplugged"
  REQUIRE(scanner.ScanAll().missing == 1);
  REQUIRE(env.games.Find("game-hollow")->status == model::GameStatus::Missing);

  fs::rename(lib / "game-hollow-away", lib / "game-hollow");  // and back
  scanner.ScanAll();
  CHECK(env.games.Find("game-hollow")->status == model::GameStatus::NeedsInstall);
  CHECK_FALSE(env.games.Find("game-hollow")->last_error.empty());
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
  REQUIRE(env.games.Find("celeste")->status == model::GameStatus::SettingUp);

  fs::rename(lib / "Celeste", lib / "Celeste-away");
  REQUIRE(scanner.ScanAll().missing == 1);
  fs::rename(lib / "Celeste-away", lib / "Celeste");
  scanner.ScanAll();

  // No runner_ref, no prefix on disk: "ready" would be a lie.
  CHECK(env.games.Find("celeste")->status == model::GameStatus::SettingUp);
}
