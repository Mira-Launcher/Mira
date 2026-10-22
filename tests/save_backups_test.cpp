#include <doctest.h>
#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <json.hpp>
#include <string>

#include "core/Files.h"
#include "core/Paths.h"
#include "library/WinePrefix.h"
#include "saves/Backups.h"
#include "saves/Resolve.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr const char* kManifest = R"yaml(---
"Saver":
  files:
    "<base>/saves/*.sav":
      tags:
        - save
)yaml";

json Get(httplib::Client& client, const std::string& path) {
  auto res = client.Get(path);
  REQUIRE(res);
  REQUIRE(res->status == 200);
  return json::parse(res->body);
}

std::size_t Count(test::LiveServer& server, std::string_view type) {
  return static_cast<std::size_t>(std::ranges::count_if(
      server.events().Since(0), [&](const model::Event& event) { return event.type == type; }));
}

// A native game whose saves are <base>/saves/*.sav, listed as "Saver".
struct SaverGame {
  test::LiveServer& server;
  httplib::Client client;
  fs::path folder;

  explicit SaverGame(test::LiveServer& live) : server(live), client(live.Client()) {
    const fs::path dir = server.config().File().parent_path();
    test::Touch(dir / "endpoints" / "save-locations.yaml", kManifest);
    REQUIRE(test::AwaitJob(client, client.Post("/v1/saves/manifest/update"))["state"] ==
            "finished");
    folder = dir / "games" / "saver";
    test::Touch(folder / "run.sh", "#!/bin/sh\n", /*executable=*/true);
    Add();
  }

  void Add(const json& overrides = json::object()) {
    model::Game game;
    game.id = "saver";
    game.name = "Saver";
    game.source = "manual";
    game.platform = model::Platform::Native;
    game.status = model::GameStatus::Ready;
    game.install_path = folder.string();
    game.exe_path = "run.sh";
    game.overrides = overrides;
    REQUIRE(server.games().Upsert(game).has_value());
  }

  // Launches it and waits for it to exit.
  void Play() {
    const auto exits = [&] {
      return std::ranges::count_if(server.events().Since(0), [](const model::Event& event) {
        return event.type == "game.state" && event.payload.value("state", "") == "exited";
      });
    };
    const auto before = exits();
    REQUIRE(client.Post("/v1/games/saver/launch")->status < 300);
    REQUIRE(test::WaitUntil([&] { return exits() > before; }));
  }

  json Saves() { return Get(client, "/v1/games/saver/saves"); }

  json BackUp() { return test::AwaitJob(client, client.Post("/v1/games/saver/saves/backup")); }
};

}  // namespace

TEST_CASE("Saves are backed up after play, once per change, keeping mtimes and sharing copies") {
  test::LiveServer server(test::TempDir("saves-backup"));
  SaverGame saver(server);
  const fs::path backups = saves::GameBackups(server.config(), "saver");
  REQUIRE(backups.parent_path() == fs::path(server.config().GetString("saves.backup_root")));

  test::Touch(saver.folder / "saves/slot1.sav", "one");
  test::Touch(saver.folder / "saves/slot2.sav", "two");
  const auto written =
      fs::last_write_time(saver.folder / "saves/slot1.sav") - std::chrono::hours(5);
  fs::last_write_time(saver.folder / "saves/slot1.sav", written);
  CHECK(saver.Saves()["status"] == "ready");

  saver.Play();
  REQUIRE(test::WaitUntil([&] { return Count(server, "game.saves_changed") == 1; }));
  json saves = saver.Saves();
  CHECK(saves["status"] == "kept");
  REQUIRE(saves["snapshots"].size() == 1);
  const json first = saves["snapshots"][0];
  CHECK(first["reason"] == "after_play");
  CHECK(first["files"] == 2);
  CHECK(first["bytes"] == 6);
  const fs::path copy = backups / first["id"].get<std::string>() / "files/install/saves/slot1.sav";
  CHECK(files::ReadFile(copy) == "one");
  CHECK(fs::last_write_time(copy) == written);

  // A session that changed nothing adds no snapshot; one that changed a file does, sharing the
  // unchanged file's copy.
  saver.Play();
  test::Touch(saver.folder / "saves/slot2.sav", "two!");
  saver.Play();
  REQUIRE(test::WaitUntil([&] { return Count(server, "game.saves_changed") == 2; }));
  server.services().saves.WaitIdle();
  saves = saver.Saves();
  REQUIRE(saves["snapshots"].size() == 2);
  const std::string second = saves["snapshots"][0]["id"];
  CHECK(saves["snapshots"][1]["id"] == first["id"]);
  CHECK(fs::equivalent(copy, backups / second / "files/install/saves/slot1.sav"));
  CHECK_FALSE(
      fs::equivalent(backups / first["id"].get<std::string>() / "files/install/saves/slot2.sav",
                     backups / second / "files/install/saves/slot2.sav"));
  CHECK(saves::BackupBytes(backups) == paths::TreeBytes(backups) - 3);

  // By hand: unchanged takes none; the newest saves.keep are kept.
  json job = saver.BackUp();
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["taken"] == false);
  CHECK(job["result"]["snapshot"]["id"] == second);
  REQUIRE(server.MutableConfig().Set("saves.keep", 2).has_value());
  test::Touch(saver.folder / "saves/slot3.sav", "three");
  fs::create_directories(backups / "20200101T000000Z.partial/files");  // a backup cut short
  job = saver.BackUp();
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["taken"] == true);
  CHECK(job["result"]["snapshot"]["reason"] == "manual");
  CHECK(job["result"]["snapshot"]["files"] == 3);
  saves = saver.Saves();
  REQUIRE(saves["snapshots"].size() == 2);
  CHECK(saves["snapshots"][1]["id"] == second);
  CHECK_FALSE(fs::exists(backups / first["id"].get<std::string>()));
  CHECK_FALSE(fs::exists(backups / "20200101T000000Z.partial"));
}

TEST_CASE(
    "Saves too big to back up fail with a fix, and an after-play failure is kept until fixed") {
  test::LiveServer server(test::TempDir("saves-too-large"));
  SaverGame saver(server);
  saver.Add({{"saves.max_mb", 1}});
  test::Touch(saver.folder / "saves/big.sav", std::string((1 << 20) + 1, 'x'));

  const json job = saver.BackUp();
  REQUIRE(job["state"] == "failed");
  CHECK(job["error"]["code"] == "saves_too_large");
  CHECK(job["error"]["fix"] == json{{"kind", "game"}, {"target", "saver"}, {"step", "saves"}});

  saver.Play();
  REQUIRE(test::WaitUntil([&] { return Count(server, "game.saves_changed") == 1; }));
  json saves = saver.Saves();
  CHECK(saves["snapshots"].empty());
  CHECK(saves["last_error"]["code"] == "saves_too_large");

  test::Touch(saver.folder / "saves/big.sav", "small now");
  saver.Play();
  REQUIRE(test::WaitUntil([&] { return Count(server, "game.saves_changed") == 3; }));
  saves = saver.Saves();
  CHECK(saves["snapshots"].size() == 1);
  CHECK_FALSE(saves.contains("last_error"));
  CHECK(saver.client.Post("/v1/games/nope/saves/backup")->status == 404);
}

TEST_CASE("Backups go to the first Mira folder unless a backup folder is set") {
  test::LiveServer server(test::TempDir("saves-root"));
  const fs::path dir = server.config().File().parent_path();
  SaverGame saver(server);
  test::Touch(saver.folder / "saves/slot1.sav", "one");
  REQUIRE(server.MutableConfig().Set("saves.backup_root", "").has_value());
  REQUIRE(
      server.MutableConfig()
          .Set("mira_folders", json::array({(dir / "first").string(), (dir / "second").string()}))
          .has_value());
  REQUIRE(saver.BackUp()["state"] == "finished");
  CHECK(fs::is_directory(dir / "first/saves/saver"));

  REQUIRE(
      server.MutableConfig().Set("saves.backup_root", (dir / "elsewhere").string()).has_value());
  test::Touch(saver.folder / "saves/slot1.sav", "one more");
  REQUIRE(saver.BackUp()["state"] == "finished");
  CHECK(fs::is_directory(dir / "elsewhere/saver"));
  // Older backups stay where they were made.
  CHECK(fs::is_directory(dir / "first/saves/saver"));
}

TEST_CASE("Removing a game keeps its save backups unless asked to delete them") {
  test::LiveServer server(test::TempDir("saves-removal"));
  SaverGame saver(server);
  const fs::path backups = saves::GameBackups(server.config(), "saver");
  test::Touch(saver.folder / "saves/slot1.sav", "one");
  REQUIRE(saver.BackUp()["state"] == "finished");

  const auto post = [&](const std::string& path) {
    auto res = saver.client.Post(path, R"({"ids": ["saver"]})", "application/json");
    REQUIRE(res);
    return res;
  };
  const json plans = json::parse(post("/v1/games/removal")->body);
  REQUIRE(plans.size() == 1);
  CHECK(plans[0]["saves"] == backups.string());
  const json sizes = test::AwaitJob(saver.client, post("/v1/games/removal/sizes"));
  CHECK(sizes["result"]["saver"]["saves"] == saves::BackupBytes(backups));
  CHECK(sizes["result"]["saver"]["saves"].get<std::uintmax_t>() > 3);

  // Its folder must be in a library folder for purge to delete it.
  REQUIRE(server.MutableConfig()
              .Set("library_roots", json::array({saver.folder.parent_path().string()}))
              .has_value());
  const json purged =
      test::AwaitJob(saver.client, saver.client.Delete("/v1/games/saver?purge=true"));
  INFO(purged.dump());
  REQUIRE(purged["state"] == "finished");
  CHECK_FALSE(fs::exists(saver.folder));
  CHECK(fs::exists(backups));
  saver.Add();  // added back under its id, it finds them
  CHECK(saver.Saves()["snapshots"].size() == 1);

  REQUIRE(saver.client.Delete("/v1/games/saver?delete_saves=true")->status == 200);
  CHECK_FALSE(fs::exists(backups));
}

TEST_CASE("A restore puts a backup's files and times back, after a backup that undoes it") {
  test::LiveServer server(test::TempDir("saves-restore"));
  SaverGame saver(server);
  const fs::path slot1 = saver.folder / "saves/slot1.sav";
  test::Touch(slot1, "one");
  const auto written = fs::last_write_time(slot1) - std::chrono::hours(5);
  fs::last_write_time(slot1, written);
  json job = saver.BackUp();
  REQUIRE(job["state"] == "finished");
  const std::string kept = job["result"]["snapshot"]["id"];

  const auto restore = [&](const std::string& snapshot) {
    return test::AwaitJob(
        saver.client, saver.client.Post("/v1/games/saver/saves/restore",
                                        json{{"snapshot", snapshot}}.dump(), "application/json"));
  };
  test::Touch(slot1, "one, played on");
  test::Touch(saver.folder / "saves/slot2.sav", "new");
  job = restore(kept);
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["restored"] == kept);
  CHECK(job["result"]["files"] == 1);
  CHECK(files::ReadFile(slot1) == "one");
  CHECK(fs::last_write_time(slot1) == written);
  CHECK(fs::exists(saver.folder / "saves/slot2.sav"));  // not in the backup: left alone
  const std::string undo = job["result"]["undo"];
  json saves = saver.Saves();
  REQUIRE(saves["snapshots"].size() == 2);
  CHECK(saves["snapshots"][0]["id"] == undo);
  CHECK(saves["snapshots"][0]["reason"] == "before_restore");

  job = restore(undo);
  REQUIRE(job["state"] == "finished");
  CHECK(files::ReadFile(slot1) == "one, played on");

  // Copies taken before a restore go once older than every kept backup.
  REQUIRE(server.MutableConfig().Set("saves.keep", 1).has_value());
  test::Touch(slot1, "one, played more");
  REQUIRE(saver.BackUp()["state"] == "finished");
  saves = saver.Saves();
  REQUIRE(saves["snapshots"].size() == 1);
  CHECK(saves["snapshots"][0]["reason"] == "manual");

  const std::string last = saves["snapshots"][0]["id"];
  // A backup deleted after the restore found it stops the restore before anything is replaced.
  {
    const auto found = saves::FindSnapshot(saves::GameBackups(server.config(), "saver"), last);
    REQUIRE(found);
    const auto game = server.games().Find("saver");
    REQUIRE(game);
    REQUIRE(saves::DeleteSnapshot(server.config(), "saver", last).has_value());
    test::Touch(slot1, "one, played again");
    const auto restored = saves::RestoreSnapshot(server.config(), *game, *found);
    REQUIRE_FALSE(restored);
    CHECK(restored.error().code == "snapshot_not_found");
    CHECK(files::ReadFile(slot1) == "one, played again");
    test::Touch(slot1, "one, played more");
    REQUIRE(saver.BackUp()["state"] == "finished");
  }
  const std::string last_kept = saver.Saves()["snapshots"][0]["id"];
  CHECK(saver.client
            .Post("/v1/games/saver/saves/restore", R"({"snapshot": "nope"})", "application/json")
            ->status == 404);
  CHECK(
      saver.client
          .Post("/v1/games/saver/saves/restore", R"({"snapshot": "../saver"})", "application/json")
          ->status == 404);
  REQUIRE(saver.client.Delete("/v1/games/saver/saves/" + last_kept)->status == 200);
  CHECK(saver.Saves()["snapshots"].empty());
  CHECK(saver.client.Delete("/v1/games/saver/saves/" + last_kept)->status == 404);
  CHECK(fs::exists(slot1));
}

TEST_CASE("A restore waits for the game to stop, and lands in the game's current prefix") {
  test::LiveServer server(test::TempDir("saves-restore-prefix"));
  const fs::path dir = server.config().File().parent_path();
  SaverGame saver(server);
  test::Touch(saver.folder / "saves/slot1.sav", "one");
  json job = saver.BackUp();
  REQUIRE(job["state"] == "finished");
  const std::string kept = job["result"]["snapshot"]["id"];

  // Not while it runs.
  test::Touch(saver.folder / "run.sh",
              "#!/bin/sh\nwhile [ ! -e \"$(dirname \"$0\")/stop\" ]; do sleep 0.05; done\n",
              /*executable=*/true);
  REQUIRE(saver.client.Post("/v1/games/saver/launch")->status < 300);
  REQUIRE(test::WaitUntil([&] { return server.services().supervisor.IsRunning("saver"); }));
  job = test::AwaitJob(saver.client,
                       saver.client.Post("/v1/games/saver/saves/restore",
                                         json{{"snapshot", kept}}.dump(), "application/json"));
  CHECK(job["error"]["code"] == "game_running");
  test::Touch(saver.folder / "stop");
  REQUIRE(test::WaitUntil([&] { return !server.services().supervisor.IsRunning("saver"); }));

  // A Windows game's saves follow it to another prefix, whose Windows user is Proton's.
  test::Touch(dir / "endpoints" / "save-locations.yaml", R"yaml(---
"Windowed":
  files:
    "<winAppData>/Windowed/*.sav":
      tags:
        - save
)yaml");
  REQUIRE(test::AwaitJob(saver.client, saver.client.Post("/v1/saves/manifest/update"))["state"] ==
          "finished");
  model::Game windowed;
  windowed.id = "windowed";
  windowed.name = "Windowed";
  windowed.source = "manual";
  windowed.platform = model::Platform::Windows;
  windowed.status = model::GameStatus::Ready;
  windowed.install_path = (dir / "games/windowed").string();
  windowed.data_dir = (dir / "prefixes/wine").string();
  fs::create_directories(dir / "prefixes/wine/drive_c");
  test::Touch(library::WindowsUserDir(dir / "prefixes/wine") / "AppData/Roaming/Windowed/a.sav",
              "wine");
  REQUIRE(server.games().Upsert(windowed).has_value());
  job = test::AwaitJob(saver.client, saver.client.Post("/v1/games/windowed/saves/backup"));
  REQUIRE(job["state"] == "finished");
  const std::string wine = job["result"]["snapshot"]["id"];

  windowed.data_dir = (dir / "prefixes/proton").string();
  fs::create_directories(dir / "prefixes/proton/pfx/drive_c/users/steamuser");
  REQUIRE(server.games().Upsert(windowed).has_value());
  job = test::AwaitJob(saver.client,
                       saver.client.Post("/v1/games/windowed/saves/restore",
                                         json{{"snapshot", wine}}.dump(), "application/json"));
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["undo"].is_null());  // nothing there to keep
  CHECK(files::ReadFile(dir / "prefixes/proton/pfx/drive_c/users/steamuser/AppData/Roaming/"
                              "Windowed/a.sav") == "wine");

  // A native game has no prefix to put it in.
  windowed.platform = model::Platform::Native;
  windowed.data_dir.clear();
  REQUIRE(server.games().Upsert(windowed).has_value());
  job = test::AwaitJob(saver.client,
                       saver.client.Post("/v1/games/windowed/saves/restore",
                                         json{{"snapshot", wine}}.dump(), "application/json"));
  CHECK(job["error"]["code"] == "saves_unplaceable");
}
