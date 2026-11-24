#include "library/Watcher.h"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

#include "config/Config.h"
#include "steam/SteamScanner.h"
#include "store/GameStore.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
using test::Touch;
namespace fs = std::filesystem;

namespace {

// What a test of something the watcher must not do waits for instead of a fixed time: a real game
// dropped into `root` afterwards. The watcher handles changes in order, and the scan that adds it
// covers the whole root, so once it's in the library, what came before has been dealt with.
void DropPartnerGame(const fs::path& root, const store::GameStore& games) {
  Touch(root / "Hollow Knight" / "hollow_knight", "", /*executable=*/true);
  REQUIRE(test::WaitUntil([&] { return games.Find("hollow-knight").has_value(); }));
}

}  // namespace

TEST_CASE("Watcher picks up a new game folder in each of two watched roots") {
  const fs::path games_root = TempDir("watch-games");
  const fs::path apps_root = TempDir("watch-apps");

  test::TestEnv env("watch-state");
  REQUIRE(
      env.config
          .Set("library_roots", nlohmann::json::array({games_root.string(), apps_root.string()}))
          .has_value());
  REQUIRE(env.config.Set("prefix_root", (games_root / "prefix").string()).has_value());
  REQUIRE(env.config.Set("scan.debounce_ms", 100).has_value());  // fast, this is a test
  store::GameStore& games = env.games;
  library::Watcher watcher(env.config, games, env.events);

  std::thread watcher_thread([&] { watcher.Run(); });
  REQUIRE(test::WaitUntil([&] { return watcher.RootsWatched() > 0; }));

  // Both native, so this is about two watched roots, not provisioning a prefix.
  Touch(games_root / "Celeste" / "Celeste", "", /*executable=*/true);
  Touch(apps_root / "Hollow Knight" / "hollow_knight", "", /*executable=*/true);

  CHECK(test::WaitUntil([&] { return games.Find("celeste") && games.Find("hollow-knight"); }));

  watcher.Stop();
  watcher_thread.join();

  auto celeste = games.Find("celeste");
  REQUIRE(celeste.has_value());
  CHECK(model::WaitsFor(*celeste, model::SetupStep::Confirm));
  CHECK(celeste->platform == model::Platform::Native);
  CHECK(celeste->install_path == (games_root / "Celeste").string());

  auto hollow_knight = games.Find("hollow-knight");
  REQUIRE(hollow_knight.has_value());
  CHECK(hollow_knight->platform == model::Platform::Native);
  CHECK(hollow_knight->install_path == (apps_root / "Hollow Knight").string());
}

TEST_CASE("Watcher follows library_roots after ReloadRoots") {
  test::TestEnv env("watch-reload");
  const fs::path old_root = env.dir / "old";
  const fs::path new_root = env.dir / "new";
  fs::create_directories(old_root);
  fs::create_directories(new_root);
  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({old_root.string()})));
  REQUIRE(env.config.Set("scan.debounce_ms", 100));

  library::Watcher watcher(env.config, env.games, env.events);
  std::thread watcher_thread([&] { watcher.Run(); });
  REQUIRE(test::WaitUntil([&] { return watcher.RootsWatched() > 0; }));

  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({new_root.string()})));
  watcher.ReloadRoots();
  REQUIRE(test::WaitUntil([&] { return watcher.RootsWatched() > 1; }));

  test::Touch(new_root / "Celeste" / "Celeste", "", /*executable=*/true);
  CHECK(test::WaitUntil([&] { return env.games.Find("celeste").has_value(); }));

  watcher.Stop();
  watcher_thread.join();
}

TEST_CASE("Watcher never rediscovers its own prefix directory as a game") {
  const fs::path root = TempDir("watch-prefix-regression");
  test::TestEnv env("watch-prefix-state");
  config::Config& config = env.config;
  REQUIRE(config.Set("library_roots", nlohmann::json::array({root.string()})).has_value());
  REQUIRE(config.Set("prefix_root", (root / "prefix").string()).has_value());
  REQUIRE(config.Set("scan.debounce_ms", 100).has_value());

  store::GameStore& games = env.games;
  api::EventBus& events = env.events;
  library::Watcher watcher(config, games, events);

  std::thread watcher_thread([&] { watcher.Run(); });
  REQUIRE(test::WaitUntil([&] { return watcher.RootsWatched() > 0; }));

  fs::create_directories(root / "prefix" / "celeste" / "drive_c");
  Touch(root / "prefix" / "celeste" / "system.reg");
  DropPartnerGame(root, games);

  watcher.Stop();
  watcher_thread.join();

  CHECK(games.All().size() == 1);
  CHECK_FALSE(games.Find("celeste").has_value());
}

TEST_CASE("Watcher offers a dropped archive untouched, unless another app has or extracts it") {
  const fs::path root = TempDir("watch-archive-root");
  test::TestEnv env("watch-archive-state");
  REQUIRE(env.config.Set("library_roots", nlohmann::json::array({root.string()})).has_value());
  REQUIRE(env.config.Set("prefix_root", (root / "prefix").string()).has_value());
  REQUIRE(env.config.Set("scan.debounce_ms", 100).has_value());
  library::FoundArchives archives(env.events);
  library::Watcher watcher(env.config, env.games, env.events);
  watcher.UseFoundArchives(archives);
  std::thread watcher_thread([&] { watcher.Run(); });
  REQUIRE(test::WaitUntil([&] { return watcher.RootsWatched() > 0; }));

  const fs::path staging = TempDir("watch-archive-staging");
  Touch(staging / "Celeste" / "Celeste", "", /*executable=*/true);
  const auto pack = [&](const std::string& name) {
    const fs::path made = staging / name;
    REQUIRE(std::system(
                ("cd " + staging.string() + " && 7z a -bso0 -bsp0 " + made.string() + " Celeste")
                    .c_str()) == 0);
    fs::rename(made, root / name);
    return root / name;
  };
  const fs::path archive = pack("free.zip");
  CHECK(test::WaitUntil([&] { return archives.Find(archive).has_value(); }));
  CHECK(archives.Find(archive)->taken_by.empty());
  CHECK(fs::exists(archive));
  CHECK(env.games.All().empty());

  // Someone unpacks it after all: the card goes, and the folder is the game.
  fs::copy(staging / "Celeste", root / "Celeste", fs::copy_options::recursive);
  CHECK(test::WaitUntil([&] { return !archives.Find(archive); }));
  CHECK(test::WaitUntil([&] { return env.games.Find("celeste").has_value(); }));

  // Another process that has it open and gets on with it: held back.
  const fs::path busy = staging / "busy.zip";
  {
    std::ofstream out(busy);
    out << std::string(1 << 20, 'x');
  }
  const std::string held = (root / "busy.zip").string();
  fs::rename(busy, held);
  REQUIRE(std::system(("setsid -f sh -c 'exec 3<" + held +
                       "; for i in 1 2 3 4 5 6; do head -c 1000 <&3 >/dev/null; sleep 0.5; done'")
                          .c_str()) == 0);
  CHECK(test::WaitUntil([&] {
    const auto found = archives.Find(held);
    return found && !found->taken_by.empty();
  }));
  // Done without leaving anything: offered.
  CHECK(test::WaitUntil([&] {
    const auto found = archives.Find(held);
    return found && found->taken_by.empty();
  }));

  watcher.Stop();
  watcher_thread.join();
}

TEST_CASE(
    "Watcher never extracts or scans anything under a configured runner_search_paths root, "
    "even if it overlaps a library root") {
  // A runner build being downloaded (runner/Downloader.cpp, its own
  // separate tar) into runner_search_paths must never also be treated as a
  // droppable archive or a new game folder here -- see Watcher.cpp's
  // paths::IsWithin for why. This is a real, if unusual, config: nothing
  // stops library_roots from overlapping runner_search_paths.
  const fs::path root = TempDir("watch-runner-overlap-root");
  const fs::path games_root = TempDir("watch-runner-overlap-games");
  test::TestEnv env("watch-runner-overlap-state");
  config::Config& config = env.config;
  REQUIRE(config.Set("library_roots", nlohmann::json::array({root.string(), games_root.string()}))
              .has_value());
  REQUIRE(config.Set("runner_search_paths", nlohmann::json::array({root.string()})).has_value());
  REQUIRE(config.Set("prefix_root", (root / "prefix").string()).has_value());
  REQUIRE(config.Set("scan.debounce_ms", 100).has_value());

  store::GameStore& games = env.games;
  api::EventBus& events = env.events;
  library::Watcher watcher(config, games, events);

  std::thread watcher_thread([&] { watcher.Run(); });
  REQUIRE(test::WaitUntil([&] { return watcher.RootsWatched() > 0; }));

  // A folder shaped like a Proton build, dropped exactly as
  // runner/Downloader.cpp would leave one after extracting it.
  fs::create_directories(root / "Fake-Proton-1" / "files" / "bin");
  Touch(root / "Fake-Proton-1" / "proton", "", /*executable=*/true);

  // An archive too, in case a tarball briefly exists there mid-download.
  const fs::path staging = TempDir("watch-runner-overlap-staging");
  fs::create_directories(staging / "Celeste");
  Touch(staging / "Celeste" / "Celeste", "", /*executable=*/true);
  const fs::path archive = root / "Celeste.tar.gz";
  REQUIRE(std::system(
              ("tar -C " + staging.string() + " -czf " + archive.string() + " Celeste").c_str()) ==
          0);
  // In a library folder of its own, since nothing in the runner folder is ever scanned.
  DropPartnerGame(games_root, games);

  watcher.Stop();
  watcher_thread.join();

  CHECK(fs::exists(archive));                            // untouched, not extracted
  CHECK(fs::exists(root / "Fake-Proton-1" / "proton"));  // untouched, not treated as a game
  CHECK(games.All().size() == 1);
}

TEST_CASE(
    "CreateMissingRoots creates a missing root under home, parents included, but not one outside "
    "it") {
  test::TestEnv env("watcher-create-roots");
  const fs::path home = env.dir / "home";
  const fs::path outside = env.dir / "outside";
  fs::create_directories(home);
  fs::create_directories(outside);
  REQUIRE(env.config
              .Set("library_roots", nlohmann::json::array({(home / "Mira" / "Games").string(),
                                                           (outside / "Games").string()}))
              .has_value());

  {
    test::EnvVar at_home("HOME", home.c_str());
    library::CreateMissingRoots(env.config);
  }

  CHECK(fs::is_directory(home / "Mira" / "Games"));
  CHECK_FALSE(fs::exists(outside / "Games"));
}

TEST_CASE("Watcher imports a Steam game once Steam has finished installing it") {
  test::TestEnv env("watch-steam");
  const fs::path steamapps = env.config.GetPath("steam.root") / "steamapps";
  fs::create_directories(steamapps);
  std::atomic<int> scans = 0;
  library::Watcher watcher(env.config, env.games, env.events);
  watcher.OnSteamChanged([&] {
    steam::SteamScanner scanner(env.config, env.games, env.events);
    REQUIRE(scanner.Scan().has_value());
    ++scans;
  });
  std::thread watcher_thread([&] { watcher.Run(); });
  REQUIRE(test::WaitUntil([&] { return watcher.RootsWatched() > 0; }));

  const auto write_manifest = [&](int state_flags) {
    Touch(steamapps / "appmanifest_504230.acf",
          std::format(R"("AppState" {{ "appid" "504230" "name" "Celeste" "installdir" "Celeste" )"
                      R"("StateFlags" "{}" }})",
                      state_flags));
  };
  Touch(steamapps / "common" / "Celeste" / "Celeste", "", /*executable=*/true);
  write_manifest(1026);  // downloading
  REQUIRE(test::WaitUntil([&] { return scans == 1; }));
  CHECK_FALSE(env.games.Find("steam-504230").has_value());

  write_manifest(4);  // fully installed
  CHECK(test::WaitUntil([&] { return env.games.Find("steam-504230").has_value(); }));

  watcher.Stop();
  watcher_thread.join();
}
