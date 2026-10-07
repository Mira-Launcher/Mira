#include <doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "config/Config.h"
#include "library/Watcher.h"
#include "store/GameStore.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
using test::Touch;
namespace fs = std::filesystem;

namespace {

// Waits for a "game.added" event, polling the bus rather than sleeping a
// fixed amount: debounce plus the watcher's poll tick make the exact timing
// unpredictable, so this waits for the actual signal instead.
bool WaitForGameAdded(api::EventBus& events, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    for (const model::Event& event : events.Since(0)) {
      if (event.type == "game.added") return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
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

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!(games.Find("celeste") && games.Find("hollow-knight")) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  watcher.Stop();
  watcher_thread.join();

  auto celeste = games.Find("celeste");
  REQUIRE(celeste.has_value());
  CHECK(celeste->status == model::GameStatus::Ready);
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
  CHECK(WaitForGameAdded(env.events, std::chrono::seconds(5)));

  watcher.Stop();
  watcher_thread.join();
  CHECK(env.games.Find("celeste").has_value());
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
  std::this_thread::sleep_for(std::chrono::milliseconds(800));  // past debounce, nothing should fire

  watcher.Stop();
  watcher_thread.join();

  CHECK(games.All().empty());
  CHECK(events.Since(0).empty());
}

TEST_CASE("Watcher auto-extracts a dropped archive and picks up the resulting folder as a game") {
  const fs::path root = TempDir("watch-archive-root");
  test::TestEnv env("watch-archive-state");
  config::Config& config = env.config;
  REQUIRE(config.Set("library_roots", nlohmann::json::array({root.string()})).has_value());
  REQUIRE(config.Set("prefix_root", (root / "prefix").string()).has_value());
  REQUIRE(config.Set("scan.debounce_ms", 100).has_value());
  REQUIRE(config.Set("scan.auto_extract_archives", true).has_value());

  store::GameStore& games = env.games;
  api::EventBus& events = env.events;
  library::Watcher watcher(config, games, events);

  std::thread watcher_thread([&] { watcher.Run(); });
  REQUIRE(test::WaitUntil([&] { return watcher.RootsWatched() > 0; }));

  // Build a real tar.gz containing a native game folder's shape, and drop
  // it directly into the watched root, exactly like a user extracting a
  // download by hand, except here Mira does it.
  const fs::path staging = TempDir("watch-archive-staging");
  fs::create_directories(staging / "Celeste");
  Touch(staging / "Celeste" / "Celeste", "", /*executable=*/true);
  const fs::path archive = root / "Celeste.tar.gz";
  REQUIRE(std::system(("tar -C " + staging.string() + " -czf " + archive.string() + " Celeste").c_str()) == 0);

  CHECK(WaitForGameAdded(events, std::chrono::seconds(5)));
  CHECK(test::WaitUntil([&] { return !fs::exists(archive); }));  // the archive itself is gone

  watcher.Stop();
  watcher_thread.join();
  CHECK(fs::exists(root / "Celeste" / "Celeste"));

  auto celeste = games.Find("celeste");
  REQUIRE(celeste.has_value());
  CHECK(celeste->platform == model::Platform::Native);
  CHECK(celeste->install_path == (root / "Celeste").string());
}

TEST_CASE("Watcher leaves a dropped archive alone when auto_extract_archives is off") {
  const fs::path root = TempDir("watch-archive-off-root");
  test::TestEnv env("watch-archive-off-state");
  config::Config& config = env.config;
  REQUIRE(config.Set("library_roots", nlohmann::json::array({root.string()})).has_value());
  REQUIRE(config.Set("prefix_root", (root / "prefix").string()).has_value());
  REQUIRE(config.Set("scan.debounce_ms", 100).has_value());
  REQUIRE(config.Set("scan.auto_extract_archives", false).has_value());

  store::GameStore& games = env.games;
  api::EventBus& events = env.events;
  library::Watcher watcher(config, games, events);

  std::thread watcher_thread([&] { watcher.Run(); });
  REQUIRE(test::WaitUntil([&] { return watcher.RootsWatched() > 0; }));

  const fs::path staging = TempDir("watch-archive-off-staging");
  fs::create_directories(staging / "Celeste");
  Touch(staging / "Celeste" / "Celeste", "", /*executable=*/true);
  const fs::path archive = root / "Celeste.tar.gz";
  REQUIRE(std::system(("tar -C " + staging.string() + " -czf " + archive.string() + " Celeste").c_str()) == 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(800));  // past debounce, nothing should fire

  watcher.Stop();
  watcher_thread.join();

  CHECK(fs::exists(archive));  // untouched
  CHECK(games.All().empty());
}

TEST_CASE("Watcher never extracts or scans anything under a configured runner_search_paths root, "
         "even if it overlaps a library root") {
  // A runner build being downloaded (runner/Downloader.cpp, its own
  // separate tar) into runner_search_paths must never also be treated as a
  // droppable archive or a new game folder here -- see Watcher.cpp's
  // paths::IsWithin for why. This is a real, if unusual, config: nothing
  // stops library_roots from overlapping runner_search_paths.
  const fs::path root = TempDir("watch-runner-overlap-root");
  test::TestEnv env("watch-runner-overlap-state");
  config::Config& config = env.config;
  REQUIRE(config.Set("library_roots", nlohmann::json::array({root.string()})).has_value());
  REQUIRE(config.Set("runner_search_paths", nlohmann::json::array({root.string()})).has_value());
  REQUIRE(config.Set("prefix_root", (root / "prefix").string()).has_value());
  REQUIRE(config.Set("scan.debounce_ms", 100).has_value());
  REQUIRE(config.Set("scan.auto_extract_archives", true).has_value());

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
  REQUIRE(std::system(("tar -C " + staging.string() + " -czf " + archive.string() + " Celeste").c_str()) == 0);

  std::this_thread::sleep_for(std::chrono::milliseconds(800));  // past debounce, nothing should fire

  watcher.Stop();
  watcher_thread.join();

  CHECK(fs::exists(archive));  // untouched, not extracted
  CHECK(fs::exists(root / "Fake-Proton-1" / "proton"));  // untouched, not treated as a game
  CHECK(games.All().empty());
}

TEST_CASE("CreateMissingRoots creates a missing root under home, parents included, but not one outside it") {
  test::TestEnv env("watcher-create-roots");
  const fs::path home = env.dir / "home";
  const fs::path outside = env.dir / "outside";
  fs::create_directories(home);
  fs::create_directories(outside);
  REQUIRE(env.config
              .Set("library_roots", nlohmann::json::array({(home / "Mira" / "Games").string(),
                                                            (outside / "Games").string()}))
              .has_value());

  const std::string old_home = std::getenv("HOME") != nullptr ? std::getenv("HOME") : "";
  setenv("HOME", home.c_str(), 1);
  library::CreateMissingRoots(env.config);
  setenv("HOME", old_home.c_str(), 1);

  CHECK(fs::is_directory(home / "Mira" / "Games"));
  CHECK_FALSE(fs::exists(outside / "Games"));
}
