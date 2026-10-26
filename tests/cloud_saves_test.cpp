#include <doctest.h>
#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <json.hpp>
#include <string>

#include "core/Files.h"
#include "epic/Legendary.h"
#include "gog/Gog.h"
#include "library/WinePrefix.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

fs::path EndpointDir(const char* name) {
  const std::string url = std::getenv(std::format("MIRA_{}_URL", name).c_str());
  return fs::path(url.substr(std::string_view("file://").size()));
}

json Get(httplib::Client& client, const std::string& path) {
  auto res = client.Get(path);
  REQUIRE(res);
  REQUIRE(res->status == 200);
  return json::parse(res->body);
}

int Lines(const fs::path& file, std::string_view containing) {
  std::ifstream in(file);
  int count = 0;
  for (std::string line; std::getline(in, line);)
    if (line.find(containing) != std::string::npos) ++count;
  return count;
}

void SetTime(const fs::path& file, std::chrono::seconds from_now) {
  fs::last_write_time(file, fs::file_time_type::clock::now() + from_now);
}

// A signed-in gogdl whose cloud is a folder: <dir>/gog-cloud/<location>/<file>, listed over
// file:// as GOG's cloudstorage lists it. `gogdl relist` lists it again after a test changes it;
// with <dir>/gog-cloud.fail present, transfers fail the way gogdl's do, with exit 0.
void UseFakeGogdl(test::LiveServer& server) {
  const fs::path dir = server.config().File().parent_path();
  const fs::path listing = EndpointDir("GOG_CLOUD") / "v1/4862/client1";
  test::Touch(dir / "gogdl",
              std::format(R"sh(#!/bin/sh
CLOUD='{0}'
relist() {{
  mkdir -p "$(dirname '{1}')"
  out='['; sep=''
  for f in "$CLOUD"/*/*; do
    [ -f "$f" ] || continue
    name="$(basename "$(dirname "$f")")/$(basename "$f")"
    sum=$(md5sum < "$f" | cut -c1-32)
    when=$(date -u -r "$f" +%Y-%m-%dT%H:%M:%S+00:00)
    out="$out$sep{{\"name\": \"$name\", \"hash\": \"$sum\", \"last_modified\": \"$when\"}}"
    sep=', '
  done
  printf '%s]' "$out" > '{1}'
}}
[ "$1" = "--version" ] && {{ echo 'gogdl version 1.3.0'; exit 0; }}
[ "$1" = "--auth-config-path" ] && shift 2
echo "$*" >> "$0.calls"
case "$1" in
  auth) echo '[INFO] refreshing'; echo '{{"access_token": "cloudtok", "user_id": "4862"}}' ;;
  relist) relist ;;
  save-sync)
    if [ -e "$CLOUD.fail" ]; then echo 'There was an error uploading a file' >&2; echo 1.0; exit 0; fi
    mkdir -p "$2" "$CLOUD/$9"
    case "${{10}}" in
      --force-download) cp -p "$CLOUD/$9"/* "$2"/ 2>/dev/null ;;
      --force-upload) for f in "$2"/*; do cp "$f" "$CLOUD/$9/"; done; relist ;;
    esac
    date +%s ;;
esac
)sh",
                          (dir / "gog-cloud").string(), listing.string()),
              /*executable=*/true);
  REQUIRE(server.MutableConfig().Set("gog.gogdl_bin", (dir / "gogdl").string()).has_value());
  test::Touch(gog::AuthConfigPath(server.config()),
              R"({"46899977096215655": {"access_token": "tok", "refresh_token": "ref",
                  "expires_in": 3600, "loginTime": 9999999999, "user_id": "4862"}})");
  test::Touch(
      EndpointDir("GOG_CONTENT") / "products/1207/os/windows/builds",
      json{{"items", {{{"link", "file://" + (EndpointDir("GOG_CONTENT") / "meta/1207").string()}}}}}
          .dump());
  test::Touch(EndpointDir("GOG_CONTENT") / "meta/1207",
              R"({"clientId": "client1", "clientSecret": "secret1"})");
  test::Touch(EndpointDir("GOG_REMOTE_CONFIG") / "components/galaxy_client/clients/client1",
              R"({"content": {"Windows": {"cloudStorage": {"enabled": true, "locations": [
                  {"name": "saves", "location": "<?APPLICATION_DATA_ROAMING?>/Fir"}]}}}})");
  REQUIRE(std::system((dir / "gogdl relist").c_str()) == 0);
}

model::Game WindowsStoreGame(const fs::path& dir, const std::string& source,
                             const std::string& ref) {
  model::Game game;
  game.id = source + "-" + ref;
  game.name = "Fir";
  game.source = source;
  game.source_ref = ref;
  game.platform = model::Platform::Windows;
  game.status = model::GameStatus::Ready;
  game.install_path = (dir / "games/fir").string();
  game.data_dir = (dir / "prefixes/fir").string();
  fs::create_directories(dir / "prefixes/fir/drive_c");
  return game;
}

// A signed-in legendary whose cloud is <dir>/epic-cloud: files/ holds the saves, manifests one
// line per upload. The game keeps its saves in `folder` (Epic's placeholder form). With
// epic-cloud.offline present listing fails; with epic-cloud.slow it takes 5 s.
fs::path UseFakeLegendary(test::LiveServer& server, std::string_view folder) {
  const fs::path dir = server.config().File().parent_path();
  const fs::path cloud = dir / "epic-cloud";
  test::Touch(dir / "legendary",
              std::format(R"sh(#!/bin/sh
CLOUD='{}'
case "$1" in
  --version) echo 'legendary version "0.21.1"' ;;
  info) echo '{{"game": {{"cloud_saves_supported": true, "cloud_save_folder": "{}"}}}}' ;;
  list-saves)
    [ -e "$CLOUD.offline" ] && exit 1
    [ -e "$CLOUD.slow" ] && sleep 5
    echo 'Save games:'; echo '- Fir ("Fir")'; cat "$CLOUD/manifests" 2>/dev/null | sed 's/^/ + /' ;;
  sync-saves)
    echo "$*" >> "$0.calls"
    mkdir -p "$4" "$CLOUD/files"
    case "$5" in
      --force-download) rm -rf "$4"/*; cp -p "$CLOUD/files"/* "$4"/ ;;
      --force-upload) cp "$4"/* "$CLOUD/files/"; date -u +%Y.%m.%d-%H.%M.%S.manifest >> "$CLOUD/manifests" ;;
    esac ;;
esac
)sh",
                          cloud.string(), folder),
              /*executable=*/true);
  REQUIRE(
      server.MutableConfig().Set("epic.legendary_bin", (dir / "legendary").string()).has_value());
  test::Touch(epic::LegendaryConfigDir() / "user.json",
              R"({"displayName": "Tester", "account_id": "acc1",
                  "refresh_expires_at": "2999-01-01T00:00:00Z"})");
  return cloud;
}

// An upload to Epic's cloud from another computer, `minutes` past 2200.
void UploadElsewhere(const fs::path& cloud, const std::string& save, int minutes) {
  test::Touch(cloud / "files/slot.sav", save);
  std::ofstream(cloud / "manifests", std::ios::app)
      << std::format("2200.01.01-00.{:02}.00.manifest\n", minutes);
}

struct Cloud {
  httplib::Client& client;
  std::string id;

  json Run(const std::string& action) {
    return test::AwaitJob(client, client.Post(std::format("/v1/games/{}/saves/cloud", id),
                                              json{{"action", action}}.dump(), "application/json"));
  }
  json State() { return Get(client, std::format("/v1/games/{}/saves", id))["cloud"]; }
};

}  // namespace

TEST_CASE("GOG cloud saves move the way they're behind, and a conflict moves nothing") {
  test::LiveServer server(test::TempDir("cloud-gog"));
  const fs::path dir = server.config().File().parent_path();
  UseFakeGogdl(server);
  REQUIRE(server.MutableConfig().Set("saves.cloud", true).has_value());
  const model::Game game = WindowsStoreGame(dir, "gog", "1207");
  REQUIRE(server.games().Upsert(game).has_value());
  auto client = server.Client();
  Cloud cloud{client, game.id};
  const fs::path local =
      library::WindowsUserDir(dir / "prefixes/fir") / "AppData/Roaming/Fir/slot1.sav";
  const fs::path remote = dir / "gog-cloud/saves/slot1.sav";
  const auto relist = [&] { REQUIRE(std::system((dir / "gogdl relist").c_str()) == 0); };

  CHECK(cloud.State()["state"] == "unknown");  // not asked yet
  test::Touch(local, "local 1");
  json job = cloud.Run("check");
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["state"] == "local_newer");
  CHECK(job["result"]["locations"][0]["name"] == "saves");
  CHECK_FALSE(fs::exists(remote));

  // First sync with an empty cloud: uploads.
  job = cloud.Run("sync");
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["locations"][0]["moved"] == "upload");
  CHECK(files::ReadFile(remote) == "local 1");
  json state = cloud.State();
  CHECK(state["state"] == "synced");
  CHECK(state["store"] == "GOG");
  CHECK(state["synced_at"].get<std::int64_t>() > 0);
  CHECK(cloud.Run("sync")["result"]["locations"][0].contains("moved") == false);

  // Another computer uploaded: a backup, then the download.
  test::Touch(remote, "cloud 2");
  SetTime(remote, std::chrono::seconds(60));
  relist();
  job = cloud.Run("sync");
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["locations"][0]["moved"] == "download");
  CHECK(files::ReadFile(local) == "cloud 2");
  json saves = Get(client, "/v1/games/gog-1207/saves");
  REQUIRE_FALSE(saves["snapshots"].empty());
  CHECK(saves["snapshots"][0]["reason"] == "before_cloud_download");

  // Played here since: seen without asking GOG, then uploaded.
  test::Touch(local, "local 3");
  SetTime(local, std::chrono::seconds(120));
  CHECK(cloud.State()["state"] == "local_newer");
  job = cloud.Run("sync");
  CHECK(job["result"]["locations"][0]["moved"] == "upload");
  CHECK(files::ReadFile(remote) == "local 3");

  // Both changed: nothing moves until the user picks a side.
  test::Touch(local, "local 4");
  SetTime(local, std::chrono::seconds(180));
  test::Touch(remote, "cloud 4");
  SetTime(remote, std::chrono::seconds(240));
  relist();
  const int transfers = Lines(dir / "gogdl.calls", "save-sync");
  job = cloud.Run("sync");
  REQUIRE(job["state"] == "failed");
  CHECK(job["error"]["code"] == "saves_conflict");
  CHECK(job["error"]["fix"] == json{{"kind", "game"}, {"target", "gog-1207"}, {"step", "saves"}});
  CHECK(Lines(dir / "gogdl.calls", "save-sync") == transfers);
  CHECK(files::ReadFile(local) == "local 4");
  state = cloud.State();
  CHECK(state["state"] == "conflict");
  // Each side's newest, to choose between them: this computer's file, and the cloud's upload.
  const auto seconds = [](const fs::path& file) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::file_clock::to_sys(fs::last_write_time(file)).time_since_epoch())
        .count();
  };
  CHECK(state["local_at"] == seconds(local));
  CHECK(state["cloud_at"] == seconds(remote));
  CHECK(state["files"] == 1);
  CHECK(state["bytes"] == std::string("local 4").size());

  // Taking the cloud's keeps this computer's in a backup.
  const std::size_t backups = Get(client, "/v1/games/gog-1207/saves")["snapshots"].size();
  job = cloud.Run("download");
  REQUIRE(job["state"] == "finished");
  CHECK(files::ReadFile(local) == "cloud 4");
  saves = Get(client, "/v1/games/gog-1207/saves");
  CHECK(saves["snapshots"].size() == backups + 1);
  CHECK(saves["cloud"]["state"] == "synced");

  // The game's client secret and token stay out of every event.
  for (const model::Event& event : server.events().Since(0)) {
    const std::string text = event.payload.dump();
    CHECK(text.find("secret1") == std::string::npos);
    CHECK(text.find("cloudtok") == std::string::npos);
  }
  CHECK(Get(client, "/v1/games/gog-1207/saves").dump().find("secret1") == std::string::npos);
}

TEST_CASE("A first GOG sync with saves on both sides compares them, and a failed transfer fails") {
  test::LiveServer server(test::TempDir("cloud-gog-first"));
  const fs::path dir = server.config().File().parent_path();
  UseFakeGogdl(server);
  REQUIRE(server.MutableConfig().Set("saves.cloud", true).has_value());
  const model::Game game = WindowsStoreGame(dir, "gog", "1207");
  REQUIRE(server.games().Upsert(game).has_value());
  auto client = server.Client();
  Cloud cloud{client, game.id};
  const fs::path local =
      library::WindowsUserDir(dir / "prefixes/fir") / "AppData/Roaming/Fir/slot1.sav";
  const fs::path remote = dir / "gog-cloud/saves/slot1.sav";

  // Different saves on both sides: a conflict, nothing moved.
  test::Touch(local, "mine");
  test::Touch(remote, "theirs");
  REQUIRE(std::system((dir / "gogdl relist").c_str()) == 0);
  json job = cloud.Run("sync");
  CHECK(job["error"]["code"] == "saves_conflict");
  CHECK(files::ReadFile(local) == "mine");

  // The same saves on both sides: in sync, still nothing moved.
  test::Touch(remote, "mine");
  REQUIRE(std::system((dir / "gogdl relist").c_str()) == 0);
  job = cloud.Run("sync");
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["state"] == "synced");
  CHECK(Lines(dir / "gogdl.calls", "save-sync") == 0);

  // gogdl exits 0 after a failed transfer; Mira doesn't take it as synced.
  test::Touch(local, "mine, changed");
  SetTime(local, std::chrono::seconds(60));
  test::Touch(dir / "gog-cloud.fail");
  job = cloud.Run("sync");
  REQUIRE(job["state"] == "failed");
  CHECK(job["error"]["code"] == "cloud_sync_failed");
  CHECK(cloud.State()["state"] == "local_newer");

  // A native GOG game: GOG keeps cloud saves for Windows and Mac games only.
  model::Game native = WindowsStoreGame(dir, "gog", "2000");
  native.platform = model::Platform::Native;
  native.data_dir.clear();
  REQUIRE(server.games().Upsert(native).has_value());
  Cloud other{client, native.id};
  CHECK(other.Run("check")["error"]["code"] == "cloud_unsupported");
  const json state = other.State();
  CHECK(state["state"] == "unsupported");
  CHECK_FALSE(state["reason"].get<std::string>().empty());
}

TEST_CASE("Epic cloud saves move through legendary into Mira's own save folder") {
  test::LiveServer server(test::TempDir("cloud-epic"));
  const fs::path dir = server.config().File().parent_path();
  REQUIRE(server.MutableConfig().Set("saves.cloud", true).has_value());
  const fs::path epic_cloud = UseFakeLegendary(server, "{AppData}/Fir/Saved/");
  const model::Game game = WindowsStoreGame(dir, "epic", "Fir");
  REQUIRE(server.games().Upsert(game).has_value());
  auto client = server.Client();
  Cloud cloud{client, game.id};
  const fs::path folder = library::WindowsUserDir(dir / "prefixes/fir") / "AppData/Local/Fir/Saved";

  test::Touch(folder / "slot.sav", "local 1");
  json job = cloud.Run("sync");
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["locations"][0]["moved"] == "upload");
  CHECK(Lines(dir / "legendary.calls", "--save-path " + folder.string() + " --force-upload -y") ==
        1);
  CHECK(files::ReadFile(epic_cloud / "files/slot.sav") == "local 1");
  CHECK(cloud.State()["state"] == "synced");

  // A newer upload from elsewhere comes down over the folder, after a backup.
  UploadElsewhere(epic_cloud, "cloud 2", 0);
  job = cloud.Run("sync");
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["locations"][0]["moved"] == "download");
  CHECK(files::ReadFile(folder / "slot.sav") == "cloud 2");
  CHECK(Get(client, "/v1/games/epic-Fir/saves")["snapshots"][0]["reason"] ==
        "before_cloud_download");
  fs::remove(epic::LegendaryConfigDir() / "user.json");
}

TEST_CASE("Cloud saves sync before a launch and after play, and a conflict waits for a choice") {
  test::LiveServer server(test::TempDir("cloud-launch"));
  const fs::path dir = server.config().File().parent_path();
  REQUIRE(server.MutableConfig().Set("saves.cloud", true).has_value());
  const fs::path epic_cloud = UseFakeLegendary(server, "{InstallDir}/Saved/");
  const fs::path folder = dir / "games/fir";
  const fs::path save = folder / "Saved/slot.sav";
  const fs::path seen = folder / "seen";
  // The game reads its save as it starts, then saves.
  test::Touch(folder / "run.sh",
              std::format("#!/bin/sh\ncat '{0}' > '{1}'\nprintf ' played' >> '{0}'\n",
                          save.string(), seen.string()),
              /*executable=*/true);
  model::Game game;
  game.id = "epic-Fir";
  game.name = "Fir";
  game.source = "epic";
  game.source_ref = "Fir";
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.install_path = folder.string();
  game.exe_path = "run.sh";
  // The same folder Epic names, as the user would add it.
  game.overrides["saves.extra_paths"] = json::array({"<base>/Saved"});
  REQUIRE(server.games().Upsert(game).has_value());
  auto client = server.Client();
  const auto count = [&](std::string_view type) {
    return std::ranges::count_if(server.events().Since(0), [&](const model::Event& event) {
      return event.type == type &&
             (type != "game.state" || event.payload.value("state", "") == "exited") &&
             (type != "game.saves_activity" || event.payload.value("doing", "") == "syncing");
    });
  };
  const auto launch = [&](const json& body) {
    return client.Post("/v1/games/epic-Fir/launch", body.dump(), "application/json");
  };
  // Launches, then waits for the game to exit and its saves to be synced after play.
  const auto play = [&](const json& body) {
    const auto exits = count("game.state");
    const auto launched = launch(body);
    REQUIRE(launched);
    REQUIRE(launched->status < 300);
    const auto syncs = count("game.saves_activity");
    REQUIRE(test::WaitUntil(
        [&] { return count("game.state") > exits && count("game.saves_activity") > syncs; }));
    server.services().saves.WaitIdle();
  };
  const auto reasons = [&] {
    std::vector<std::string> found;
    const json saves = Get(client, "/v1/games/epic-Fir/saves");
    for (const json& snapshot : saves["snapshots"]) found.push_back(snapshot["reason"]);
    return found;
  };

  // The first launch: this computer's saves go up, are backed up first, and again after play.
  test::Touch(save, "local 1");
  play(json::object());
  CHECK(files::ReadFile(seen) == "local 1");
  CHECK(files::ReadFile(epic_cloud / "files/slot.sav") == "local 1 played");
  CHECK(std::ranges::contains(reasons(), "first"));
  CHECK(std::ranges::contains(reasons(), "after_play"));
  const json locations = Get(client, "/v1/games/epic-Fir/saves")["locations"];
  REQUIRE(locations.size() == 1);
  CHECK(locations[0]["path"] == (folder / "Saved").string());

  // Played on another computer: the game starts with that one's saves.
  UploadElsewhere(epic_cloud, "cloud 2", 0);
  play(json::object());
  CHECK(files::ReadFile(seen) == "cloud 2");
  CHECK(files::ReadFile(epic_cloud / "files/slot.sav") == "cloud 2 played");

  // Both changed: the game doesn't start until the user picks.
  const auto conflict = [&](const std::string& here, const std::string& there, int minutes) {
    test::Touch(save, here);
    SetTime(save, std::chrono::minutes(minutes));
    UploadElsewhere(epic_cloud, there, minutes);
  };
  conflict("local 3", "cloud 3", 1);
  const auto notices = count("notification");
  const auto refused = launch(json::object());
  REQUIRE(refused);
  CHECK(refused->status == 409);
  const json error = json::parse(refused->body)["error"];
  CHECK(error["code"] == "saves_conflict");
  CHECK(error["fix"] == json{{"kind", "game"}, {"target", "epic-Fir"}, {"step", "saves"}});
  CHECK(files::ReadFile(seen) == "cloud 2");
  REQUIRE(count("notification") == notices + 1);
  const auto notice = test::WaitForEvent(server.events(), "notification");
  REQUIRE(notice);
  CHECK(notice->payload["id"] == "epic-Fir");
  CHECK(notice->payload["persistent"] == true);
  CHECK(launch({{"saves", "keep_cloud"}})->status == 400);

  // Taking the cloud's keeps this computer's in a backup.
  play({{"saves", "use_cloud"}});
  CHECK(files::ReadFile(seen) == "cloud 3");
  CHECK(std::ranges::contains(reasons(), "before_cloud_download"));
  conflict("local 4", "cloud 4", 2);
  play({{"saves", "keep_local"}});
  CHECK(files::ReadFile(seen) == "local 4");
  CHECK(files::ReadFile(epic_cloud / "files/slot.sav") == "local 4 played");

  // Played without syncing: the conflict is still there after play, and the user is told.
  conflict("local 5", "cloud 5", 3);
  const int transfers = Lines(dir / "legendary.calls", "sync-saves");
  const auto before_skip = count("notification");
  play({{"saves", "skip"}});
  CHECK(files::ReadFile(seen) == "local 5");
  CHECK(Lines(dir / "legendary.calls", "sync-saves") == transfers);
  CHECK(count("notification") == before_skip + 1);
  REQUIRE(test::AwaitJob(client,
                         client.Post("/v1/games/epic-Fir/saves/cloud", R"({"action": "upload"})",
                                     "application/json"))["state"] == "finished");

  // Offline, or too slow: the game starts with the saves here.
  test::Touch(dir / "epic-cloud.offline");
  play(json::object());
  CHECK(files::ReadFile(seen) == "local 5 played");
  fs::remove(dir / "epic-cloud.offline");
  REQUIRE(server.MutableConfig().Set("saves.cloud_timeout_s", 1).has_value());
  test::Touch(dir / "epic-cloud.slow");
  const auto exits = count("game.state");
  const auto started = std::chrono::steady_clock::now();
  const auto slow = launch(json::object());
  REQUIRE(slow);
  CHECK(slow->status < 300);
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(4));

  // Played again while the slow sync after play still holds the game: the launch waits for it.
  REQUIRE(test::WaitUntil([&] { return count("game.state") > exits; }));
  const auto again = launch({{"saves", "skip"}});
  REQUIRE(again);
  CHECK(again->status < 300);
  fs::remove(dir / "epic-cloud.slow");
  REQUIRE(test::WaitUntil([&] { return !server.services().supervisor.IsRunning("epic-Fir"); }));
  server.services().saves.WaitIdle();
  fs::remove(epic::LegendaryConfigDir() / "user.json");
}
