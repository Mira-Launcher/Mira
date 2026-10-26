#include <doctest.h>
#include <httplib.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <json.hpp>
#include <string>
#include <vector>

#include "epic/Legendary.h"
#include "gog/Gog.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// Valve's binary KeyValues, as Steam writes its stats files.
std::string Key(char type, const std::string& name) {
  return std::string(1, type) + name + std::string(1, '\0');
}
std::string Object(const std::string& name, const std::string& body) {
  return Key('\0', name) + body + std::string(1, '\x08');
}
std::string Text(const std::string& name, const std::string& text) {
  return Key('\x01', name) + text + std::string(1, '\0');
}
std::string Int(const std::string& name, std::uint32_t value) {
  std::string bytes = Key('\x02', name);
  for (int i = 0; i < 4; ++i) bytes += static_cast<char>((value >> (8 * i)) & 0xff);
  return bytes;
}
std::string File(const std::string& body) {
  return body + std::string(1, '\x08');
}

// The folder an Isolate'd server's $MIRA_<name>_URL points at.
fs::path EndpointDir(const char* name) {
  const std::string url = std::getenv(std::format("MIRA_{}_URL", name).c_str());
  return fs::path(url.substr(std::string_view("file://").size()));
}

constexpr std::uint64_t kSteamId = 76561197960265728ULL + 1000;

// Steam app 440 with three achievements, the account having unlocked the first and third.
void WriteSteamStats(const fs::path& steam_root) {
  const std::string bits =
      Object("0", Text("name", "FIRST") +
                      Object("display", Object("name", Text("english", "First steps") +
                                                           Text("token", "FIRST_NAME")) +
                                            Object("desc", Text("english", "Start the game")) +
                                            Int("hidden", 0) + Text("icon", "first.jpg") +
                                            Text("icon_gray", "first_gray.jpg"))) +
      Object("1", Text("name", "SECRET") +
                      Object("display", Object("name", Text("english", "Secret ending")) +
                                            Object("desc", Text("english", "See the other end")) +
                                            Int("hidden", 1) + Text("icon", "secret.jpg"))) +
      Object("2", Text("name", "POLYGLOT") +
                      Object("display", Object("name", Text("english", "Polyglot") +
                                                           Text("german", "Vielsprachig")) +
                                            Object("desc", Text("english", "Say hello") +
                                                               Text("german", "Sag hallo")) +
                                            Int("hidden", 0)));
  const std::string schema = File(Object(
      "440", Object("stats", Object("1", Object("bits", bits) + Text("type", "ACHIEVEMENTS")) +
                                 Object("3", Text("type", "INT")))));
  const std::string progress = File(Object(
      "cache", Int("crc", 7) + Object("1", Int("data", 0b101) + Object("AchievementTimes",
                                                                       Int("0", 1700000000) +
                                                                           Int("2", 1700000500)))));
  fs::create_directories(steam_root / "steamapps");
  test::Touch(steam_root / "appcache" / "stats" / "UserGameStatsSchema_440.bin", schema);
  test::Touch(steam_root / "appcache" / "stats" / "UserGameStats_1000_440.bin", progress);
}

model::Game SteamGame(const std::string& appid) {
  model::Game game;
  game.id = "steam-" + appid;
  game.name = "Game " + appid;
  game.source = "steam";
  game.source_ref = appid;
  game.runner_ref = "steam:" + appid;
  game.status = model::GameStatus::Ready;
  return game;
}

int EventCount(const api::EventBus& events, const std::string& type) {
  return static_cast<int>(std::ranges::count_if(
      events.Since(0), [&](const model::Event& event) { return event.type == type; }));
}

}  // namespace

TEST_CASE("A Steam game's achievements come from Steam's stats files, in its language") {
  test::LiveServer server(test::TempDir("achievements-steam"));
  REQUIRE(server.MutableConfig().Set("achievements.enabled", true).has_value());
  REQUIRE(server.MutableConfig().Set("steam.steamid64", std::to_string(kSteamId)).has_value());
  const test::EnvVar all("LC_ALL", nullptr);
  const test::EnvVar messages("LC_MESSAGES", nullptr);
  const test::EnvVar lang("LANG", "de_DE.UTF-8");
  WriteSteamStats(server.config().GetString("steam.root"));
  test::Touch(
      EndpointDir("STEAM_API") / "ISteamUserStats" / "GetGlobalAchievementPercentagesForApp" / "v2",
      R"({"achievementpercentages": {"achievements": [
                   {"name": "FIRST", "percent": "89.7"}, {"name": "SECRET", "percent": 3.5}]}})");
  REQUIRE(server.games().Upsert(SteamGame("440")).has_value());
  auto client = server.Client();

  // Nothing is known before the first check.
  auto none = client.Get("/v1/games/steam-440/achievements");
  REQUIRE(none);
  CHECK(none->status == 404);
  CHECK(json::parse(none->body)["error"]["code"] == "no_achievements");

  const json job = test::AwaitJob(client, client.Post("/v1/games/steam-440/achievements/refresh"));
  REQUIRE(job["state"] == "finished");
  CHECK(job["result"]["total"] == 3);
  CHECK(job["result"]["unlocked"] == 2);
  const auto ready = test::WaitForEvent(server.events(), "game.achievements_ready");
  REQUIRE(ready);
  CHECK(ready->payload == json{{"id", "steam-440"}, {"total", 3}, {"unlocked", 2}});

  auto got = client.Get("/v1/games/steam-440/achievements");
  REQUIRE(got);
  REQUIRE(got->status == 200);
  const json set = json::parse(got->body);
  CHECK(set["source"] == "steam");
  CHECK(set["total"] == 3);
  CHECK(set["unlocked"] == 2);
  REQUIRE(set["items"].size() == 3);
  const json& first = set["items"][0];
  const json& secret = set["items"][1];
  const json& polyglot = set["items"][2];
  CHECK(first["key"] == "FIRST");
  CHECK(first["name"] == "First steps");  // no German text: English
  CHECK(first["unlocked"] == true);
  CHECK(first["unlocked_at"] == 1700000000);
  CHECK(first["rarity"].get<double>() == doctest::Approx(89.7));
  CHECK(secret["hidden"] == true);
  CHECK(secret["unlocked"] == false);
  CHECK(secret["unlocked_at"] == 0);
  CHECK(secret["rarity"].get<double>() == doctest::Approx(3.5));
  CHECK(polyglot["name"] == "Vielsprachig");
  CHECK(polyglot["description"] == "Sag hallo");
  CHECK(polyglot["unlocked_at"] == 1700000500);
  CHECK(polyglot["rarity"] == -1);
  CHECK(polyglot["icon"] == "");

  // The same answer again isn't announced.
  const json again =
      test::AwaitJob(client, client.Post("/v1/games/steam-440/achievements/refresh"));
  CHECK(again["result"]["changed"] == false);
  CHECK(EventCount(server.events(), "game.achievements_ready") == 1);

  // Icons are fetched on first ask, fitted, and kept.
  const fs::path icons = EndpointDir("STEAM_ICONS") / "440";
  fs::create_directories(icons);
  std::vector<unsigned char> pixels(300 * 300 * 3, 200);
  REQUIRE(stbi_write_png((icons / "first.jpg").c_str(), 300, 300, 3, pixels.data(), 0) != 0);
  auto icon = client.Get("/v1/games/steam-440/achievements/icon?key=FIRST&state=unlocked");
  REQUIRE(icon);
  REQUIRE(icon->status == 200);
  int width = 0;
  int height = 0;
  int channels = 0;
  REQUIRE(stbi_info_from_memory(reinterpret_cast<const unsigned char*>(icon->body.data()),
                                static_cast<int>(icon->body.size()), &width, &height,
                                &channels) != 0);
  CHECK(width == 128);
  CHECK(height == 128);
  fs::remove(icons / "first.jpg");
  auto cached = client.Get("/v1/games/steam-440/achievements/icon?key=FIRST&state=unlocked");
  REQUIRE(cached);
  CHECK(cached->status == 200);
  CHECK(cached->body == icon->body);
  auto missing = client.Get("/v1/games/steam-440/achievements/icon?key=FIRST&state=locked");
  REQUIRE(missing);
  CHECK(missing->status == 502);

  // Turned off, they aren't shown, and turned on again they're back as they were.
  REQUIRE(server.MutableConfig().Set("achievements.enabled", false).has_value());
  for (const char* path : {"/v1/games/steam-440/achievements",
                           "/v1/games/steam-440/achievements/icon?key=FIRST&state=unlocked"}) {
    auto off = client.Get(path);
    REQUIRE(off);
    CHECK(off->status == 400);
    CHECK(json::parse(off->body)["error"]["code"] == "achievements_off");
  }
  REQUIRE(server.MutableConfig().Set("achievements.enabled", true).has_value());
  auto back = client.Get("/v1/games/steam-440/achievements");
  REQUIRE(back);
  CHECK(back->status == 200);
  CHECK(json::parse(back->body)["total"] == 3);

  // Forgetting the game's metadata forgets its achievements and their icons.
  server.games().Metadata().Remove("steam-440");
  auto forgotten = client.Get("/v1/games/steam-440/achievements");
  REQUIRE(forgotten);
  CHECK(forgotten->status == 404);
  CHECK_FALSE(fs::exists(server.games().Metadata().ArtworkDir("steam-440") / "achievements"));
}

TEST_CASE("Achievements that can't be read say why and where to fix it") {
  test::LiveServer server(test::TempDir("achievements-errors"));
  // A Steam install that never ran the game, so the machine's own Steam isn't looked in.
  fs::create_directories(fs::path(server.config().GetString("steam.root")) / "steamapps");
  REQUIRE(server.games().Upsert(SteamGame("620")).has_value());
  model::Game own;
  own.id = "own";
  own.name = "Own";
  own.source = "manual";
  own.status = model::GameStatus::Ready;
  REQUIRE(server.games().Upsert(own).has_value());
  auto client = server.Client();

  // Turned off, a check is refused with the setting that turns it on.
  auto off = client.Post("/v1/games/steam-620/achievements/refresh");
  REQUIRE(off);
  CHECK(off->status == 400);
  CHECK(json::parse(off->body)["error"]["fix"]["target"] == "achievements.enabled");

  REQUIRE(server.MutableConfig().Set("achievements.enabled", true).has_value());
  // No stats files and no Web API key.
  const json failed =
      test::AwaitJob(client, client.Post("/v1/games/steam-620/achievements/refresh"));
  REQUIRE(failed["state"] == "failed");
  CHECK(failed["error"]["code"] == "no_local_achievements");
  CHECK(failed["error"]["fix"] == json{{"kind", "setting"}, {"target", "steam.web_api_key"}});

  // A game from a source without achievements.
  auto own_answer = client.Get("/v1/games/own/achievements");
  REQUIRE(own_answer);
  CHECK(own_answer->status == 400);
  CHECK(json::parse(own_answer->body)["error"]["code"] == "no_achievements_source");
}

TEST_CASE("A locked hidden game's achievements stay out of answers and events") {
  test::LiveServer server(test::TempDir("achievements-hidden"));
  REQUIRE(server.MutableConfig().Set("achievements.enabled", true).has_value());
  REQUIRE(server.MutableConfig().Set("steam.steamid64", std::to_string(kSteamId)).has_value());
  WriteSteamStats(server.config().GetString("steam.root"));
  model::Game game = SteamGame("440");
  game.tags = {"hidden"};
  REQUIRE(server.games().Upsert(game).has_value());
  auto client = server.Client();
  const json password = {{"password", "hunter2"}};
  REQUIRE(client.Put("/v1/hidden/password", password.dump(), "application/json")->status == 200);
  REQUIRE(client.Post("/v1/hidden/lock")->status == 200);

  auto refresh = client.Post("/v1/games/steam-440/achievements/refresh");
  REQUIRE(refresh);
  CHECK(refresh->status == 404);
  // A check after the game exits still runs, unannounced.
  REQUIRE(server.services().RefreshAchievements("steam-440").has_value());
  CHECK(EventCount(server.events(), "game.achievements_ready") == 0);
  auto listed = client.Get("/v1/games/steam-440/achievements");
  REQUIRE(listed);
  CHECK(listed->status == 404);
  CHECK(json::parse(listed->body)["error"]["code"] == "game_not_found");
}

namespace {

// A stand-in `legendary`, signed in, whose `achievements` prints `answer` (and logs each call to
// <bin>.calls) and caches `definitions` as legendary's metadata for the game, as the real one does.
void UseFakeLegendary(config::Config& config, const fs::path& dir, const std::string& answer,
                      const json& definitions = nullptr) {
  const fs::path bin = dir / "legendary";
  const fs::path meta = epic::LegendaryMetadataFile("Fir");
  fs::create_directories(meta.parent_path());
  test::Touch(dir / "legendary-answer", answer);
  test::Touch(
      dir / "legendary-meta",
      definitions.is_null()
          ? std::string("{}")
          : json{{"app_name", "Fir"}, {"achievements", {{"achievements", definitions}}}}.dump());
  test::Touch(bin,
              "#!/bin/sh\n"
              "case \"$1\" in\n"
              "  achievements)\n"
              "    echo \"$*\" >> \"$0.calls\"\n"
              "    cp \"" +
                  (dir / "legendary-meta").string() + "\" \"" + meta.string() +
                  "\"\n"
                  "    cat \"" +
                  (dir / "legendary-answer").string() +
                  "\" ;;\n"
                  "  --version) echo 'legendary version \"0.21.1\"' ;;\n"
                  "esac\n",
              /*executable=*/true);
  REQUIRE(config.Set("epic.legendary_bin", bin.string()).has_value());
  const fs::path user = epic::LegendaryConfigDir() / "user.json";
  fs::create_directories(user.parent_path());
  test::Touch(user, R"({"displayName": "Tester", "refresh_expires_at": "2999-01-01T00:00:00Z"})");
}

int Calls(const fs::path& dir) {
  std::ifstream log(dir / "legendary.calls");
  return static_cast<int>(
      std::count(std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>(), '\n'));
}

// An installed store game that runs a script natively.
model::Game StoreGame(const std::string& source, const std::string& ref, const fs::path& folder) {
  test::Touch(folder / "run.sh", "#!/bin/sh\n", /*executable=*/true);
  model::Game game;
  game.id = source + "-" + ref;
  game.name = ref;
  game.source = source;
  game.source_ref = ref;
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.install_path = folder.string();
  game.exe_path = "run.sh";
  return game;
}

json Definition(const std::string& name, bool hidden) {
  return {{"achievement",
           {{"name", name},
            {"hidden", hidden},
            {"unlockedDisplayName", name + " unlocked"},
            {"unlockedDescription", name + " done"},
            {"unlockedIconLink", "https://cdn.example/" + name + ".png"},
            {"lockedIconLink", "https://cdn.example/" + name + "-locked.png"}}}};
}

}  // namespace

TEST_CASE("An Epic game's achievements come from legendary, in Epic's order") {
  test::LiveServer server(test::TempDir("achievements-epic"));
  const fs::path dir = server.config().File().parent_path();
  REQUIRE(server.MutableConfig().Set("achievements.enabled", true).has_value());
  // legendary's own groups: completed, then untouched, then hidden, each sorted its own way.
  const json answer = {{"completed",
                        {{{"name", "won"},
                          {"display_name", "Winner"},
                          {"description", "Win"},
                          {"unlocked", true},
                          {"unlock_date", "2024-05-01 12:00:00+00:00"},
                          {"rarity", {{"percent", 12.5}}},
                          {"xp", 10},
                          {"icon_link", "https://cdn.example/won.png"}}}},
                       {"in_progress", json::array()},
                       {"uninitiated",
                        {{{"name", "start"},
                          {"display_name", "Starter"},
                          {"unlocked", false},
                          {"xp", 5},
                          {"rarity", {{"percent", 90.0}}}}}},
                       {"hidden",
                        {{{"name", "secret"},
                          {"display_name", "Hidden"},
                          {"description", "Keep playing"},
                          {"unlocked", false},
                          {"xp", 50}}}}};
  UseFakeLegendary(server.MutableConfig(), dir, "[cli] INFO: noise\n" + answer.dump() + "\n",
                   json::array({Definition("start", false), Definition("secret", true),
                                Definition("won", false)}));
  REQUIRE(server.games().Upsert(StoreGame("epic", "Fir", dir / "fir")).has_value());
  auto client = server.Client();

  const json job = test::AwaitJob(client, client.Post("/v1/games/epic-Fir/achievements/refresh"));
  REQUIRE(job["state"] == "finished");
  const json set = json::parse(client.Get("/v1/games/epic-Fir/achievements")->body);
  CHECK(set["source"] == "epic");
  CHECK(set["unlocked"] == 1);
  REQUIRE(set["items"].size() == 3);
  CHECK(set["items"][0]["key"] == "start");
  CHECK(set["items"][0]["rarity"].get<double>() == doctest::Approx(90.0));
  CHECK(set["items"][1]["key"] == "secret");
  CHECK(set["items"][1]["hidden"] == true);
  CHECK(set["items"][1]["name"] == "secret unlocked");  // the client masks it, not mirad
  CHECK(set["items"][1]["xp"] == 50);
  CHECK(set["items"][2]["key"] == "won");
  CHECK(set["items"][2]["unlocked"] == true);
  CHECK(set["items"][2]["unlocked_at"] == 1714564800);
  CHECK(set["items"][2]["icon"] != "");

  // A game without Epic achievements is an empty list, not a failure.
  test::Touch(dir / "legendary-answer", "[cli] INFO: No achievements found for \"Fir\"\n");
  const json none = test::AwaitJob(client, client.Post("/v1/games/epic-Fir/achievements/refresh"));
  REQUIRE(none["state"] == "finished");
  CHECK(none["result"]["total"] == 0);

  // A legendary from before `achievements` says to update it.
  test::Touch(dir / "legendary",
              "#!/bin/sh\necho \"legendary: error: argument subparser_name: invalid choice: "
              "'achievements'\" >&2\nexit 2\n",
              /*executable=*/true);
  const json old = test::AwaitJob(client, client.Post("/v1/games/epic-Fir/achievements/refresh"));
  REQUIRE(old["state"] == "failed");
  CHECK(old["error"]["code"] == "legendary_too_old");
  CHECK(old["error"]["fix"] == json{{"kind", "source"}, {"target", "epic"}, {"step", "setup"}});
}

TEST_CASE("A GOG game's achievements come from GOG with the signed-in account") {
  test::LiveServer server(test::TempDir("achievements-gog"));
  const fs::path dir = server.config().File().parent_path();
  REQUIRE(server.MutableConfig().Set("achievements.enabled", true).has_value());
  test::Touch(dir / "gogdl", "#!/bin/sh\necho 'gogdl version 1.3.0'\n", /*executable=*/true);
  REQUIRE(server.MutableConfig().Set("gog.gogdl_bin", (dir / "gogdl").string()).has_value());
  test::Touch(gog::AuthConfigPath(server.config()),
              R"({"46899977096215655": {"access_token": "tok", "refresh_token": "ref",
                  "expires_in": 3600, "loginTime": 9999999999, "user_id": "4862"}})");
  test::Touch(
      EndpointDir("GOG_GAMEPLAY") / "clients" / "1207" / "users" / "4862" / "achievements",
      // GOG sets page_token on every reply, the whole list included. "open" is listed twice
      // to check a repeated key shows once.
      R"({"total_count": 2, "limit": 1000, "page_token": "MTAwMA", "achievements_mode": "all",
                  "items": [
                    {"achievement_id": "1", "achievement_key": "open", "name": "Opened",
                     "description": "Open the door", "image_url_unlocked": "https://img/u.png",
                     "image_url_locked": "https://img/l.png", "visible": true,
                     "date_unlocked": "2021-05-03T19:31:29+0000", "rarity": 41.2},
                    {"achievement_id": "2", "achievement_key": "twist", "name": "Twist",
                     "description": "It was you", "image_url_unlocked": "https://img/t.png",
                     "image_url_locked": "https://img/tl.png", "visible": false,
                     "date_unlocked": null, "rarity": 2.5},
                    {"achievement_id": "1", "achievement_key": "open", "name": "Opened"}]})");
  REQUIRE(server.games().Upsert(StoreGame("gog", "1207", dir / "door")).has_value());
  auto client = server.Client();

  const json job = test::AwaitJob(client, client.Post("/v1/games/gog-1207/achievements/refresh"));
  REQUIRE(job["state"] == "finished");
  const json set = json::parse(client.Get("/v1/games/gog-1207/achievements")->body);
  CHECK(set["source"] == "gog");
  CHECK(set["total"] == 2);
  REQUIRE(set["items"].size() == 2);
  CHECK(set["items"][0]["key"] == "open");
  CHECK(set["items"][0]["unlocked"] == true);
  CHECK(set["items"][0]["unlocked_at"] == 1620070289);
  CHECK(set["items"][0]["rarity"].get<double>() == doctest::Approx(41.2));
  CHECK(set["items"][1]["hidden"] == true);
  CHECK(set["items"][1]["unlocked"] == false);

  // Signed out: the fix is signing in again.
  fs::remove(gog::AuthConfigPath(server.config()));
  const json out = test::AwaitJob(client, client.Post("/v1/games/gog-1207/achievements/refresh"));
  REQUIRE(out["state"] == "failed");
  CHECK(out["error"]["fix"] == json{{"kind", "source"}, {"target", "gog"}, {"step", "login"}});

  // A cache holding repeats, as an earlier build wrote it, still shows each once.
  json doubled = server.games().Metadata().ReadAchievements("gog-1207");
  doubled["items"].push_back(doubled["items"][0]);
  doubled["items"].push_back(doubled["items"][1]);
  REQUIRE(server.games()
              .Metadata()
              .WriteAchievements("gog-1207", doubled["fetched_at"].get<std::int64_t>(), doubled)
              .has_value());
  const json cached = json::parse(client.Get("/v1/games/gog-1207/achievements")->body);
  CHECK(cached["total"] == 2);
  CHECK(cached["items"].size() == 2);
}

TEST_CASE("Achievements are checked after playing, but not every time for a game without any") {
  test::LiveServer server(test::TempDir("achievements-after-play"));
  const fs::path dir = server.config().File().parent_path();
  REQUIRE(server.MutableConfig().Set("achievements.enabled", true).has_value());
  const json answer = {
      {"completed", {{{"name", "won"}, {"display_name", "Winner"}, {"unlocked", true}}}}};
  UseFakeLegendary(server.MutableConfig(), dir, answer.dump() + "\n");
  REQUIRE(server.games().Upsert(StoreGame("epic", "Fir", dir / "fir")).has_value());
  auto client = server.Client();

  const auto play = [&] {
    const std::size_t exits = static_cast<std::size_t>(
        std::ranges::count_if(server.events().Since(0), [](const model::Event& event) {
          return event.type == "game.state" && event.payload.value("state", "") == "exited";
        }));
    REQUIRE(client.Post("/v1/games/epic-Fir/launch")->status < 300);
    REQUIRE(test::WaitUntil([&] {
      return static_cast<std::size_t>(
                 std::ranges::count_if(server.events().Since(0), [](const model::Event& event) {
                   return event.type == "game.state" &&
                          event.payload.value("state", "") == "exited";
                 })) > exits;
    }));
  };

  play();
  const auto ready = test::WaitForEvent(server.events(), "game.achievements_ready");
  REQUIRE(ready);
  CHECK(ready->payload["unlocked"] == 1);
  CHECK(Calls(dir) == 1);

  // Found to have none: the next session doesn't ask Epic again.
  test::Touch(dir / "legendary-answer", "[cli] INFO: No achievements found for \"Fir\"\n");
  REQUIRE(test::AwaitJob(client, client.Post("/v1/games/epic-Fir/achievements/refresh"))["state"] ==
          "finished");
  CHECK(Calls(dir) == 2);
  play();
  server.services().achievements.WaitIdle();
  CHECK(Calls(dir) == 2);
}
