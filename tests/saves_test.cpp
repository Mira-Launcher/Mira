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
