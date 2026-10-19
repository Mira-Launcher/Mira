#include <doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "core/Strings.h"
#include "library/AutoInstall.h"
#include "proc/Session.h"
#include "runner/IRunner.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

fs::path TempFile(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / "auto-install";
  fs::create_directories(dir);
  const fs::path file = dir / name;
  fs::remove(file);
  return file;
}

void Write(const fs::path& path, std::string_view head, std::string_view tail, size_t padding) {
  std::ofstream out(path, std::ios::binary);
  out << head;
  out << std::string(padding, '\0');
  out << tail;
}

}  // namespace

TEST_CASE("A game installed elsewhere takes the installed folder's name unless it was renamed") {
  model::Game game;
  game.install_path = "/games/setup_clustertruck";
  game.name = strings::CleanGameName("setup_clustertruck");
  library::AdoptInstallFolder(game, "/prefixes/ct/drive_c/GOG Games/ClusterTruck");
  CHECK(game.name == strings::CleanGameName("ClusterTruck"));
  CHECK(game.install_path == "/prefixes/ct/drive_c/GOG Games/ClusterTruck");
  CHECK(game.installer_dir == "/games/setup_clustertruck");

  model::Game renamed;
  renamed.install_path = "/games/setup_clustertruck";
  renamed.name = "My Trucks";
  library::AdoptInstallFolder(renamed, "/prefixes/ct/drive_c/GOG Games/ClusterTruck");
  CHECK(renamed.name == "My Trucks");

  // Named before CleanGameName dropped "setup" and capitalised.
  model::Game older;
  older.install_path = "/games/setup_crate_escape_(64bit)";
  older.name = "setup crate escape (64bit)";
  library::AdoptInstallFolder(older, "/prefixes/ce/drive_c/GOG Games/Crate Escape");
  CHECK(older.name == "Crate Escape");
}

TEST_CASE("A program installed under a publisher's folder is found in its own folder") {
  const fs::path prefix = test::TempDir("auto-install-publisher-prefix");
  const fs::path ubisoft = prefix / "drive_c" / "Program Files" / "Ubisoft";
  test::Touch(ubisoft / "Ubisoft Game Launcher" / "data" / "cache.bin", "cache");
  test::Touch(ubisoft / "Crate Escape" / "bin" / "CrateEscape.exe", "game");
  test::Touch(ubisoft / "Crate Escape" / "unins000.dat", "uninstall");
  config::Config config(prefix / "settings.toml");
  config.Load();
  test::Isolate(config);

  const auto installed = library::NewInstall(config, prefix, {});
  REQUIRE(installed);
  CHECK(installed->dir == ubisoft / "Crate Escape");
  CHECK(installed->exe_path == "bin/CrateEscape.exe");
}

TEST_CASE("DetectInstallerFormat recognizes Inno Setup near the head") {
  const fs::path file = TempFile("inno-head.exe");
  Write(file, "junk...Inno Setup junk", "", 0);
  CHECK(library::DetectInstallerFormat(file) == library::InstallerFormat::kInnoSetup);
}

TEST_CASE("DetectInstallerFormat recognizes NSIS's marker near the tail of a large file") {
  const fs::path file = TempFile("nsis-tail.exe");
  // Bigger than the 2MB sniff window on each end, marker only in the tail --
  // regression coverage for "don't just read the head of a monolithic
  // installer".
  Write(file, std::string(64, 'a'), "...Nullsoft...", 3 * 1024 * 1024);
  CHECK(library::DetectInstallerFormat(file) == library::InstallerFormat::kNsis);
}

TEST_CASE("DetectInstallerFormat reports unknown for a non-installer exe") {
  const fs::path file = TempFile("plain.exe");
  Write(file, "just a normal game binary, nothing special here", "", 0);
  CHECK(library::DetectInstallerFormat(file) == library::InstallerFormat::kUnknown);
}

TEST_CASE("MSI installers are detected by extension and run through msiexec") {
  const fs::path msi = TempFile("setup.msi");
  Write(msi, "Inno Setup", "", 0);
  CHECK(library::DetectInstallerFormat(msi) == library::InstallerFormat::kMsi);

  const fs::path text = TempFile("readme.txt");
  Write(text, "Inno Setup", "", 0);
  CHECK(library::DetectInstallerFormat(text) == library::InstallerFormat::kUnknown);

  CHECK(runner::WindowsProgram("/g/setup.msi") ==
        std::vector<std::string>{"msiexec", "/i", "/g/setup.msi"});
  CHECK(runner::WindowsProgram("/g/start.BAT") ==
        std::vector<std::string>{"cmd", "/c", "/g/start.BAT"});
  CHECK(runner::WindowsProgram("/g/game.exe") == std::vector<std::string>{"/g/game.exe"});
}

namespace {

// A game waiting on `installer`, a shell script standing in for the .exe. Tests run
// Windows games natively, so it runs as itself with the arguments Wine would get.
model::Game AddWaitingGame(test::LiveServer& server, const fs::path& folder,
                           const std::string& script) {
  test::Touch(folder / "setup_celeste.exe", "#!/bin/sh\n" + script, /*executable=*/true);
  model::Game game;
  game.id = "celeste";
  game.name = strings::CleanGameName(folder.filename().string());
  game.platform = model::Platform::Windows;
  game.status = model::GameStatus::NeedsInstall;
  game.install_path = folder.string();
  game.exe_path = "setup_celeste.exe";
  game.data_dir = (folder.parent_path() / "prefix").string();
  fs::create_directories(fs::path(game.data_dir) / "drive_c");
  REQUIRE(server.games().Upsert(game));
  return game;
}

}  // namespace

TEST_CASE("A recognised installer runs quietly into its prefix, logging as it goes") {
  const fs::path state = test::TempDir("auto-install-quiet");
  test::LiveServer server(state);
  const fs::path release = state / "release";
  // "Inno Setup" in the file is what marks it as one; it installs where /DIR= says, C: being the
  // prefix's drive_c, and waits for `release` so the test can read the log meanwhile.
  const model::Game waiting =
      AddWaitingGame(server, state / "setup_celeste",
                     "# Inno Setup\necho \"$@\" > \"$0.args\"\n"
                     "cat /proc/self/cgroup > \"$0.cgroup\"\n"
                     "for arg; do case \"$arg\" in\n"
                     "  /DIR=C:*) dir=" +
                         (state / "prefix" / "drive_c").string() +
                         "/$(printf '%s' \"${arg#/DIR=C:}\" | tr '\\\\' /) ;;\n"
                         "  /LOG=C:*) echo 'Inno: extracting' > " +
                         (state / "prefix" / "drive_c").string() +
                         "/\"$(printf '%s' \"${arg#/LOG=C:}\" | tr '\\\\' /)\" ;;\n"
                         "esac; done\n"
                         "echo unpacking fg-01.bin\n"
                         "while [ ! -e '" +
                         release.string() +
                         "' ]; do sleep 0.05; done\n"
                         "mkdir -p \"$dir\"\nprintf game > \"$dir/Celeste.exe\"\n");
  REQUIRE(server.MutableConfig().Set("install.inno_args", "/QUIET /SP-"));
  REQUIRE(server.MutableConfig().Set("install.show_progress", false));
  httplib::Client client = server.Client();

  auto started = client.Post("/v1/games/celeste/install");
  REQUIRE(started != nullptr);
  CHECK(started->status == 202);

  // The game's log window shows the installer's output, live, while it runs.
  nlohmann::json shown_log;
  for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
       std::chrono::steady_clock::now() < deadline;
       std::this_thread::sleep_for(std::chrono::milliseconds(50))) {
    auto log = client.Get("/v1/logs/game:celeste");
    REQUIRE(log != nullptr);
    shown_log = nlohmann::json::parse(log->body);
    if (shown_log["lines"].dump().contains("unpacking fg-01.bin")) break;
  }
  CHECK(shown_log["lines"].dump().contains("unpacking fg-01.bin"));
  CHECK(shown_log["active"] == true);
  test::Touch(release);
  REQUIRE(test::WaitForEvent(server.events(), "game.install.finished"));

  const fs::path target = state / "prefix" / "drive_c" / "Games" / waiting.name;
  const auto game = server.games().Find("celeste");
  REQUIRE(game);
  CHECK(game->status == model::GameStatus::Ready);
  CHECK(game->install_path == target.string());
  CHECK(game->exe_path == "Celeste.exe");
  CHECK(game->installer_dir == (state / "setup_celeste").string());
  std::ifstream args(state / "setup_celeste" / "setup_celeste.exe.args");
  std::string passed;
  std::getline(args, passed);
  // The configured arguments, then the target and Inno's own log.
  CHECK(passed == "/QUIET /SP- /DIR=C:\\Games\\" + waiting.name + " /LOG=C:\\mira-install.log");
  // Its own scope, so running out of memory takes the installer, not mirad.
  const auto cgroup_of = [](const fs::path& file) {
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    return line;
  };
  const std::string installer_cgroup =
      cgroup_of(state / "setup_celeste" / "setup_celeste.exe.cgroup");
  CHECK(installer_cgroup.ends_with(".scope"));
  CHECK(installer_cgroup != cgroup_of("/proc/self/cgroup"));
  auto ended = client.Get("/v1/logs/game:celeste");
  REQUIRE(ended != nullptr);
  const auto ended_log = nlohmann::json::parse(ended->body);
  CHECK(ended_log["lines"].dump().contains("exited with code 0"));
  CHECK(ended_log["lines"].dump().contains("Inno: extracting"));
  CHECK(ended_log["active"] == false);

  auto progress = client.Get("/v1/games/celeste/install/progress");
  REQUIRE(progress != nullptr);
  const auto shown = nlohmann::json::parse(progress->body);
  CHECK(shown["state"] == "finished");
  CHECK(shown["mode"] == "silent");

  auto again = client.Post("/v1/games/celeste/install");
  REQUIRE(again != nullptr);
  CHECK(again->status == 409);
}

TEST_CASE("An installer that puts the game in its prefix moves the game there, renamed to match") {
  const fs::path state = test::TempDir("auto-install-into-prefix");
  test::LiveServer server(state);
  // Offered for deleting only in a library root.
  const fs::path root = state / "Games";
  fs::create_directories(root);
  REQUIRE(server.MutableConfig().Set("library_roots", nlohmann::json::array({root.string()})));
  const fs::path installed = root / "prefix" / "drive_c" / "Program Files" / "Celeste Classic";
  AddWaitingGame(server, root / "setup_celeste_1.4",
                 "mkdir -p '" + installed.string() + "'\nprintf game > '" + installed.string() +
                     "/Celeste.exe'\n");
  // Packed beside the installer, as a repack's checksum tool is: not what it installed.
  test::Touch(state / "setup_celeste_1.4" / "MD5" / "QuickSFV.exe", "tool");
  httplib::Client client = server.Client();

  auto started = client.Post("/v1/games/celeste/install");
  REQUIRE(started != nullptr);
  REQUIRE(test::WaitForEvent(server.events(), "game.install.finished"));

  const auto game = server.games().Find("celeste");
  REQUIRE(game);
  CHECK(game->status == model::GameStatus::Ready);
  CHECK(game->install_path == installed.string());
  CHECK(game->exe_path == "Celeste.exe");
  CHECK(game->name == "Celeste Classic");
  CHECK(game->installer_dir == (root / "setup_celeste_1.4").string());
  const auto leftover = test::WaitForEvent(server.events(), "game.installer_leftover");
  REQUIRE(leftover);
  CHECK(leftover->payload["installer_dir"] == (root / "setup_celeste_1.4").string());
}

TEST_CASE("An installer that uses a publisher's folder moves the game to the game's own folder") {
  const fs::path state = test::TempDir("auto-install-publisher");
  test::LiveServer server(state);
  const fs::path game_dir =
      state / "prefix" / "drive_c" / "Program Files" / "Ubisoft" / "Crate Escape";
  AddWaitingGame(server, state / "setup_crate_escape",
                 "mkdir -p '" + game_dir.string() + "/bin'\nprintf game > '" + game_dir.string() +
                     "/bin/CrateEscape.exe'\nprintf x > '" + game_dir.string() +
                     "/unins000.dat'\n");
  httplib::Client client = server.Client();

  auto started = client.Post("/v1/games/celeste/install");
  REQUIRE(started != nullptr);
  REQUIRE(test::WaitForEvent(server.events(), "game.install.finished"));

  const auto game = server.games().Find("celeste");
  REQUIRE(game);
  CHECK(game->install_path == game_dir.string());
  CHECK(game->exe_path == "bin/CrateEscape.exe");
  CHECK(game->name == "Crate Escape");
}

TEST_CASE("An installer that installs nothing leaves the game waiting, with a way forward") {
  const fs::path state = test::TempDir("auto-install-nothing");
  test::LiveServer server(state);
  AddWaitingGame(server, state / "setup_celeste", "echo installer ran\nexit 0\n");
  const fs::path log_file = proc::GameLogPath(server.games().Dir(), "celeste");
  test::Touch(log_file, "[mira-run] session start, game_id=celeste\nlast launch\n");
  httplib::Client client = server.Client();

  REQUIRE(client.Post("/v1/games/celeste/install"));
  const auto failed = test::WaitForEvent(server.events(), "game.install.failed");
  REQUIRE(failed);
  CHECK(failed->payload["code"] == "no_executable");
  CHECK(failed->payload["fix"]["step"] == "exe");
  const auto game = server.games().Find("celeste");
  CHECK(game->status == model::GameStatus::NeedsInstall);
  CHECK_FALSE(game->last_error.empty());

  // Trying again adds to the install's log, so the first run's output is still there to read; the
  // last launch's log is kept beside it.
  const auto read = [](const fs::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };
  REQUIRE(client.Post("/v1/games/celeste/install"));
  const auto failures = [&] {
    return std::ranges::count(server.events().Since(0), std::string("game.install.failed"),
                              &model::Event::type);
  };
  for (const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
       failures() < 2 && std::chrono::steady_clock::now() < deadline;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(failures() == 2);
  const std::string log = read(log_file);
  size_t runs = 0;
  for (size_t at = log.find("installer ran"); at != std::string::npos;
       at = log.find("installer ran", at + 1)) {
    ++runs;
  }
  CHECK(runs == 2);
  CHECK(read(log_file.string() + ".1").contains("last launch"));
  CHECK_FALSE(log.contains("last launch"));
}
