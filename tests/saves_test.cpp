#include <doctest.h>
#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "saves/Manifest.h"
#include "saves/Match.h"
#include "saves/Resolve.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// Entries in the shapes Ludusavi's manifest uses.
constexpr const char* kManifest = R"yaml(---
"Hollow Knight":
  files:
    "<home>/AppData/LocalLow/Team Cherry/Hollow Knight/*.dat":
      tags:
        - save
      when:
        - os: windows
    "<xdgConfig>/unity3d/Team Cherry/Hollow Knight/*.dat":
      tags:
        - save
      when:
        - os: linux
  installDir:
    Hollow Knight: {}
  steam:
    id: 367520
  gog:
    id: 1308320804
"Hollow Knight Demo":
  alias: "Hollow Knight"
"Clustertruck":
  registry:
    HKEY_CURRENT_USER/Software/Landfall Games/Clustertruck:
      tags:
        - save
  steam:
    id: 397950
"Launch Info Only":
  installDir:
    Launch Info Only: {}
  steam:
    id: 1000
"Alias Of Nothing":
  alias: "Launch Info Only"
"Needy Streamer Overload":
  files:
    "<base>/Windose_Data/Settings.es3":
      tags:
        - save
  steam:
    id: 1451940
"Baldur's Gate: Enhanced Edition":
  files:
    "<winDocuments>/Baldur's Gate - Enhanced Edition/save":
      tags:
        - save
  id:
    lutris: bgee-lutris
    steamExtra:
      - 228280
"Baldur's Gate II":
  files:
    "<winDocuments>/Baldur's Gate II - Enhanced Edition/save":
      tags:
        - save
)yaml";

void WriteManifest(const fs::path& dir, const std::string& text) {
  test::Touch(dir / "endpoints" / "save-locations.yaml", text);
}

json Get(httplib::Client& client, const std::string& path) {
  auto res = client.Get(path);
  REQUIRE(res);
  REQUIRE(res->status == 200);
  return json::parse(res->body);
}

std::vector<std::string> Names(const json& answer) {
  std::vector<std::string> names;
  for (const json& entry : answer["entries"]) names.push_back(entry["name"]);
  return names;
}

model::Game Game(std::string name) {
  model::Game game;
  game.id = "game";
  game.name = std::move(name);
  game.source = "manual";
  return game;
}

}  // namespace

TEST_CASE("The save-location list is indexed once per change, and a broken one keeps the last") {
  test::LiveServer server(test::TempDir("saves-manifest"));
  const fs::path dir = server.config().File().parent_path();
  auto client = server.Client();
  CHECK(Get(client, "/v1/saves/manifest")["games"] == 0);
  const auto announced = [&] {
    return std::ranges::count_if(server.events().Since(0), [](const model::Event& event) {
      return event.type == "saves.manifest_changed";
    });
  };

  WriteManifest(dir, kManifest);
  json job = test::AwaitJob(client, client.Post("/v1/saves/manifest/update"));
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["changed"] == true);
  CHECK(announced() == 1);
  // Hollow Knight, Clustertruck, Needy Streamer Overload and the two Baldur's Gates: the entry
  // without save locations and the alias of it are left out.
  CHECK(job["result"]["games"] == 5);
  const json state = Get(client, "/v1/saves/manifest");
  CHECK(state["games"] == 5);
  CHECK(state["fetched_at"].get<std::int64_t>() > 0);
  CHECK(state["url"] == server.config().GetString("saves.manifest_url"));
  CHECK(Names(Get(client, "/v1/saves/entries?q=launch")).empty());
  CHECK(Names(Get(client, "/v1/saves/entries?q=nothing")).empty());

  job = test::AwaitJob(client, client.Post("/v1/saves/manifest/update"));
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["changed"] == false);
  CHECK(announced() == 1);

  WriteManifest(dir, std::string(kManifest) + "\"Celeste\":\n  files:\n    \"<base>/Saves\": {}\n");
  job = test::AwaitJob(client, client.Post("/v1/saves/manifest/update"));
  CHECK(job["result"]["changed"] == true);
  CHECK(announced() == 2);
  CHECK(Names(Get(client, "/v1/saves/entries?q=celeste")) == std::vector<std::string>{"Celeste"});

  WriteManifest(dir, "<html>Too many requests</html>");
  job = test::AwaitJob(client, client.Post("/v1/saves/manifest/update"));
  REQUIRE(job["state"] == "failed");
  CHECK(job["error"]["code"] == "manifest_invalid");
  CHECK(announced() == 2);
  CHECK(Get(client, "/v1/saves/manifest")["games"] == 6);
  CHECK(Names(Get(client, "/v1/saves/entries?q=celeste")) == std::vector<std::string>{"Celeste"});
}

TEST_CASE("Searching the list fetches it when due, and a failed fetch isn't retried at once") {
  test::LiveServer server(test::TempDir("saves-search"));
  const fs::path dir = server.config().File().parent_path();
  auto client = server.Client();
  const auto jobs_failed = [&] {
    return std::ranges::count_if(server.events().Since(0), [](const model::Event& event) {
      return event.type == "job.failed" && event.payload.value("kind", "") == "saves";
    });
  };

  // No list there yet: the search answers empty and the fetch fails in the background.
  CHECK(Names(Get(client, "/v1/saves/entries?q=hollow")).empty());
  REQUIRE(test::WaitForEvent(server.events(), "job.failed"));
  CHECK(jobs_failed() == 1);
  WriteManifest(dir, kManifest);
  CHECK(Names(Get(client, "/v1/saves/entries?q=hollow")).empty());
  server.services().saves.WaitIdle();
  CHECK(std::ranges::count_if(server.events().Since(0), [](const model::Event& event) {
          return event.type == "job.started" && event.payload.value("kind", "") == "saves";
        }) == 1);

  // Asked for by hand, it's fetched; then a search ranks names starting with the words first.
  REQUIRE(test::AwaitJob(client, client.Post("/v1/saves/manifest/update"))["state"] == "finished");
  CHECK(Names(Get(client, "/v1/saves/entries?q=Baldur%27s%20Gate")) ==
        std::vector<std::string>{"Baldur's Gate II", "Baldur's Gate: Enhanced Edition"});
  const json demo = Get(client, "/v1/saves/entries?q=hollow%20knight%20demo")["entries"];
  REQUIRE(demo.size() == 1);
  CHECK(demo[0]["alias_of"] == "Hollow Knight");
}

TEST_CASE("A game's entry is found by its pick, store ids, Lutris slug, then exact name") {
  test::TestEnv env("saves-match");
  store::MetadataStore& cache = env.games.Metadata();
  const auto entries = saves::ParseManifest(kManifest);
  REQUIRE(entries.has_value());
  REQUIRE(cache.ReplaceSaveEntries(*entries).has_value());
  const auto find = [&](const model::Game& game) {
    const auto match = saves::FindEntry(env.config, cache, game);
    return match ? std::format("{} by {}", match->entry.name, match->rule) : std::string("none");
  };

  model::Game steam = Game("Something Else Entirely");
  steam.runner_ref = "steam:367520";
  CHECK(find(steam) == "Hollow Knight by steam");
  steam.runner_ref = "steam:228280";  // an extra id
  CHECK(find(steam) == "Baldur's Gate: Enhanced Edition by steam");
  // The entry holding only launch info is left out, so its game falls through to its name.
  steam.runner_ref = "steam:1000";
  steam.name = "Clustertruck";
  CHECK(find(steam) == "Clustertruck by name");

  model::Game gog = Game("Hollow Knight (GOG)");
  gog.source = "gog";
  gog.source_ref = "1308320804";
  CHECK(find(gog) == "Hollow Knight by gog");

  model::Game picked = steam;
  picked.overrides["saves.manifest_name"] = "Hollow Knight Demo";  // an alias, followed
  CHECK(find(picked) == "Hollow Knight by setting");
  picked.overrides["saves.manifest_name"] = "Renamed Since";  // gone: the other rules decide
  CHECK(find(picked) == "Clustertruck by name");

  model::Game other = Game("Something");
  other.overrides["metadata.steam_appid"] = 1451940;
  CHECK(find(other) == "Needy Streamer Overload by steam");

  model::Game lutris = Game("BG");
  lutris.source = "lutris";
  lutris.source_ref = "bgee-lutris";
  CHECK(find(lutris) == "Baldur's Gate: Enhanced Edition by lutris");
  model::Game claimed = Game("Needy Girl Overdose");  // moved out of Lutris's folders
  claimed.source_ref = "lutris:needy-streamer-overload";
  CHECK(find(claimed) == "Needy Streamer Overload by lutris");

  CHECK(find(Game("hollow knight")) == "Hollow Knight by name");
  CHECK(find(Game("Baldurs Gate - Enhanced Edition™")) ==
        "Baldur's Gate: Enhanced Edition by name");
  CHECK(find(Game("Hollow Knight Silksong")) == "none");
  CHECK(find(Game("Launch Info Only")) == "none");
}

TEST_CASE("Reads go on while the save index is rebuilt, seeing the old one or the new one") {
  test::TestEnv env("saves-rebuild");
  store::MetadataStore& cache = env.games.Metadata();
  const auto build = [](const std::string& tag) {
    std::vector<store::MetadataStore::SaveEntry> entries;
    for (int i = 0; i < 20000; ++i) {
      const std::string name = std::format("Game {} {}", i, tag);
      entries.push_back({name, saves::Normalize(name), "", "{}", {{"steam", std::to_string(i)}}});
    }
    return entries;
  };
  const auto old_entries = build("old");
  REQUIRE(cache.ReplaceSaveEntries(old_entries).has_value());

  std::atomic<bool> done = false;
  std::atomic<int> reads = 0;
  std::atomic<int> missing = 0;
  std::thread reader([&] {
    while (!done) {
      const std::string name = cache.SaveEntryWithId("steam", "19999");
      if (name != "Game 19999 old" && name != "Game 19999 new") ++missing;
      ++reads;
    }
  });
  const auto new_entries = build("new");
  const auto replaced = cache.ReplaceSaveEntries(new_entries);
  done = true;
  reader.join();
  REQUIRE(replaced.has_value());
  CHECK(reads > 0);
  CHECK(missing == 0);
  CHECK(cache.SaveEntryWithId("steam", "19999") == "Game 19999 new");
  CHECK(cache.SaveEntryCount() == 20000);
  CHECK_FALSE(cache.FindSaveEntry("Game 0 old"));
}

namespace {

model::Game WindowsGame(const fs::path& prefix, const fs::path& install) {
  model::Game game = Game("Windows Game");
  game.platform = model::Platform::Windows;
  game.data_dir = prefix.string();
  game.install_path = install.string();
  return game;
}

std::vector<std::string> Found(const saves::Location& location) {
  std::vector<std::string> files;
  for (const auto& file : location.files) files.push_back(file.root + ":" + file.path);
  return files;
}

}  // namespace

TEST_CASE("Save locations resolve in a game's prefix, by any placeholder form, ignoring case") {
  test::TestEnv env("saves-resolve-prefix");
  const test::EnvVar user("USER", "mira-player");
  const fs::path prefix = env.dir / "prefix";
  const fs::path roaming = prefix / "drive_c/users/mira-player/AppData/Roaming";
  test::Touch(roaming / "Studio/Game/save1.sav", "one");
  test::Touch(roaming / "Studio/Game/options.ini");
  const fs::path local = prefix / "drive_c/users/mira-player/AppData/Local";
  test::Touch(local / "Studio/Slot [1]/data.bin");
  const model::Game game = WindowsGame(prefix, env.dir / "install");
  test::Touch(env.dir / "install/Saves/slot.sav");
  saves::Roots roots = saves::RootsFor(env.config, game);

  // Wine's user folder, named for $USER, while there's no steamuser.
  saves::Location found = saves::Find("<winAppData>/studio/game/*.SAV", roots);
  CHECK(Found(found) ==
        std::vector<std::string>{"prefix_user:AppData/Roaming/Studio/Game/save1.sav"});
  REQUIRE(found.files.size() == 1);
  CHECK(found.files[0].bytes == 3);
  CHECK(saves::FromUnixNs(found.files[0].mtime_ns) ==
        fs::last_write_time(roaming / "Studio/Game/save1.sav"));

  // The list's, GOG's and Epic's names for local AppData are one folder; "[[]" is a literal '['.
  const std::vector<std::string> slot = {"prefix_user:AppData/Local/Studio/Slot [1]/data.bin"};
  CHECK(Found(saves::Find("<winLocalAppData>/Studio/Slot [[]1]", roots)) == slot);
  CHECK(Found(saves::Find("<?APPLICATION_DATA_LOCAL?>\\Studio\\Slot [[]1]", roots)) == slot);
  CHECK(Found(saves::Find("{AppData}/Studio/Slot [[]1]", roots)) == slot);
  CHECK(Found(saves::Find("<base>/saves", roots)) ==
        std::vector<std::string>{"install:Saves/slot.sav"});

  // Once Proton made steamuser, that's the user folder, and a file is kept relative to it.
  test::Touch(prefix / "drive_c/users/steamuser/AppData/Roaming/Studio/Game/save2.sav");
  roots = saves::RootsFor(env.config, game);
  CHECK(Found(saves::Find("<winAppData>/Studio/Game/*.sav", roots)) ==
        std::vector<std::string>{"prefix_user:AppData/Roaming/Studio/Game/save2.sav"});

  CHECK(saves::Find("<xdgData>/game", roots).problem == "unknown_placeholder");
  CHECK(saves::Find("<notAFolder>/game", roots).problem == "unknown_placeholder");
}

TEST_CASE("Save locations never take a whole home, prefix or user folder, nor follow links out") {
  test::TestEnv env("saves-resolve-safety");
  const fs::path home = env.dir / "home";
  const test::EnvVar home_var("HOME", home.c_str());
  const test::EnvVar data("XDG_DATA_HOME", nullptr);
  const test::EnvVar user("USER", "mira-player");
  test::Touch(home / ".local/share/game/save.dat");
  test::Touch(home / "Documents/letter.txt");
  test::Touch(home / "elsewhere/secret.txt");
  test::Touch(home / ".local/share/game/sub/inner.dat");
  fs::create_directory_symlink(home / "elsewhere", home / ".local/share/game/linked");

  model::Game native = Game("Native Game");
  native.platform = model::Platform::Native;
  native.install_path = (env.dir / "native").string();
  const saves::Roots roots = saves::RootsFor(env.config, native);
  const saves::Location found = saves::Find("<xdgData>/game", roots);
  CHECK(Found(found) == std::vector<std::string>{"home:.local/share/game/save.dat",
                                                 "home:.local/share/game/sub/inner.dat"});
  CHECK(found.skipped == std::vector<std::string>{"home:.local/share/game/linked"});
  CHECK(saves::Find("<home>/**", roots).problem == "too_broad");
  CHECK(saves::Find("<home>", roots).problem == "too_broad");
  CHECK(saves::Find("<xdgData>/*", roots).problem == "too_broad");
  CHECK(saves::Find("<winAppData>/game", roots).problem == "unknown_placeholder");

  // Wine links a prefix's Documents to the home folder's; taking it whole would take that.
  const fs::path prefix = env.dir / "prefix";
  fs::create_directories(prefix / "drive_c/users/mira-player");
  fs::create_directory_symlink(home / "Documents", prefix / "drive_c/users/mira-player/Documents");
  fs::create_directories(prefix / "dosdevices");
  fs::create_directory_symlink("/", prefix / "dosdevices/z:");
  test::Touch(prefix / "drive_c/Games/Thing/save.dat");
  const saves::Roots wine =
      saves::RootsFor(env.config, WindowsGame(prefix, prefix / "drive_c/Games/Thing"));
  CHECK(saves::Find("<winDocuments>", wine).problem == "too_broad");
  CHECK(saves::Find("<home>/*", wine).problem == "too_broad");
  CHECK(Found(saves::Find("<winDocuments>/letter.txt", wine)) ==
        std::vector<std::string>{"prefix_user:Documents/letter.txt"});
  // `**` walks the game's folder and never into dosdevices.
  const saves::Location deep = saves::Find("<root>/**/save.dat", wine);
  CHECK(deep.problem.empty());
  CHECK(Found(deep) == std::vector<std::string>{"install:save.dat"});
}

TEST_CASE("A Steam game's saves resolve in its compatdata prefix, its library and Steam's folder") {
  test::TestEnv env("saves-resolve-steam");
  const fs::path steam = env.dir / "steam";  // Isolate's steam.root
  const fs::path library = env.dir / "library";
  const fs::path install = library / "steamapps/common/Hollow";
  test::Touch(install / "Saves/user1.dat");
  const fs::path compat = library / "steamapps/compatdata/367520";
  test::Touch(compat /
              "pfx/drive_c/users/steamuser/AppData/LocalLow/Team Cherry/Hollow Knight/user1.dat");
  test::Touch(steam / "userdata/1000/367520/remote/cloud.dat");
  fs::create_directories(steam / "steamapps");  // what makes it a Steam folder
  model::Game game = WindowsGame(compat, install);
  game.source = "steam";
  game.runner_ref = "steam:367520";
  const saves::Roots roots = saves::RootsFor(env.config, game);

  CHECK(
      Found(saves::Find("<home>/AppData/LocalLow/Team Cherry/Hollow Knight", roots)) ==
      std::vector<std::string>{"prefix_user:AppData/LocalLow/Team Cherry/Hollow Knight/user1.dat"});
  CHECK(Found(saves::Find("<root>/steamapps/common/Hollow/Saves", roots)) ==
        std::vector<std::string>{"install:Saves/user1.dat"});
  CHECK(Found(saves::Find("<root>/userdata/<storeUserId>/<storeGameId>/remote", roots)) ==
        std::vector<std::string>{"steam:userdata/1000/367520/remote/cloud.dat"});
}

TEST_CASE("Only the list's file locations for this game's system and store are used") {
  const json entry = json::parse(R"({"files": {
    "<base>/any": {},
    "<base>/saves": {"tags": ["save"], "when": [{"os": "windows"}]},
    "<base>/linux": {"tags": ["save"], "when": [{"os": "linux"}]},
    "<base>/steam-only": {"tags": ["save"], "when": [{"os": "windows", "store": "steam"}]},
    "<base>/settings.ini": {"tags": ["config"]}
  }})");
  saves::Roots roots;
  roots.windows = true;
  roots.store = "gog";
  CHECK(saves::FileLocations(entry, roots) ==
        std::vector<std::string>{"<base>/any", "<base>/saves"});
  roots.store.clear();  // a game whose store isn't known may be any store's copy
  CHECK(saves::FileLocations(entry, roots) ==
        std::vector<std::string>{"<base>/any", "<base>/saves", "<base>/steam-only"});
  roots.windows = false;
  CHECK(saves::FileLocations(entry, roots) ==
        std::vector<std::string>{"<base>/any", "<base>/linux"});
}

TEST_CASE("A game's saves say where they are, or why there are none to keep") {
  test::LiveServer server(test::TempDir("saves-overview"));
  const fs::path dir = server.config().File().parent_path();
  const test::EnvVar user("USER", "mira-player");
  WriteManifest(dir, std::string(kManifest) + R"yaml("Wandering Sword":
  files:
    "<winAppData>/Wandering/Save":
      tags:
        - save
  cloud:
    steam: true
  steam:
    id: 1873140
)yaml");
  auto client = server.Client();
  REQUIRE(test::AwaitJob(client, client.Post("/v1/saves/manifest/update"))["state"] == "finished");

  const auto add = [&](model::Game game, const std::string& id) {
    game.id = id;
    game.status = model::GameStatus::Ready;
    REQUIRE(server.games().Upsert(game).has_value());
  };
  const fs::path prefix = dir / "prefixes/hollow";
  test::Touch(
      prefix / "drive_c/users/mira-player/AppData/LocalLow/Team Cherry/Hollow Knight/user1.dat",
      "12345");
  model::Game hollow = WindowsGame(prefix, dir / "games/hollow");
  hollow.name = "Hollow Knight";
  add(hollow, "hollow");
  json saves = Get(client, "/v1/games/hollow/saves");
  CHECK(saves["status"] == "ready");
  CHECK(saves["enabled"] == true);
  CHECK(saves["entry"] == "Hollow Knight");
  CHECK(saves["match"] == "name");
  REQUIRE(saves["locations"].size() == 1);  // its Linux location doesn't apply under Wine
  CHECK(saves["locations"][0]["files"] == 1);
  CHECK(saves["locations"][0]["bytes"] == 5);

  model::Game fresh = WindowsGame(dir / "prefixes/fresh", dir / "games/fresh");
  fresh.name = "Hollow Knight";
  add(fresh, "fresh");
  CHECK(Get(client, "/v1/games/fresh/saves")["status"] == "no_files");

  model::Game truck = WindowsGame(dir / "prefixes/truck", dir / "games/truck");
  truck.name = "Clustertruck";
  add(truck, "truck");
  CHECK(Get(client, "/v1/games/truck/saves")["status"] == "registry_only");

  model::Game unknown = Game("Not In The List");
  add(unknown, "unknown");
  saves = Get(client, "/v1/games/unknown/saves");
  CHECK(saves["status"] == "no_entry");
  CHECK(saves["entry"].is_null());

  // The user's own location makes a game without an entry one with saves.
  test::Touch(dir / "games/unknown/save.dat");
  unknown.install_path = (dir / "games/unknown").string();
  unknown.overrides["saves.extra_paths"] = json::array({"<base>/save.dat"});
  add(unknown, "unknown");
  CHECK(Get(client, "/v1/games/unknown/saves")["status"] == "ready");

  // Steam keeps a Steam Cloud game's saves while Steam starts it, unless told otherwise.
  model::Game sword = WindowsGame(dir / "steam/steamapps/compatdata/1873140",
                                  dir / "steam/steamapps/common/Wandering Sword");
  sword.name = "Wandering Sword";
  sword.source = "steam";
  sword.runner_ref = "steam:1873140";
  add(sword, "steam-1873140");
  CHECK(Get(client, "/v1/games/steam-1873140/saves")["status"] == "steam_cloud");
  sword.overrides["steam.launch_mode"] = "direct";
  add(sword, "steam-1873140");
  CHECK(Get(client, "/v1/games/steam-1873140/saves")["status"] == "no_files");
  sword.overrides = json::object({{"saves.skip_cloud", false}});
  add(sword, "steam-1873140");
  CHECK(Get(client, "/v1/games/steam-1873140/saves")["status"] == "no_files");
  sword.overrides = json::object({{"saves.enabled", false}});
  add(sword, "steam-1873140");
  saves = Get(client, "/v1/games/steam-1873140/saves");
  CHECK(saves["status"] == "off");
  CHECK(saves["enabled"] == false);

  CHECK(client.Get("/v1/games/nope/saves")->status == 404);
}

TEST_CASE("Path components match globs with classes, literal brackets and folded case") {
  CHECK(saves::MatchComponent("*.sav", "slot1.SAV", true));
  CHECK_FALSE(saves::MatchComponent("*.sav", "slot1.SAV", false));
  CHECK(saves::MatchComponent("slot[0-9].dat", "slot7.dat", false));
  CHECK_FALSE(saves::MatchComponent("slot[!0-9].dat", "slot7.dat", false));
  CHECK(saves::MatchComponent("Slot [[]1]", "Slot [1]", false));
  CHECK(saves::MatchComponent("a?c*", "abcdef", false));
  CHECK_FALSE(saves::MatchComponent("a?c", "abcd", false));
}
