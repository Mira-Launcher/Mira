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

// Sets an environment variable for one test, restoring it afterwards.
class EnvVar {
 public:
  EnvVar(const char* name, const char* value) : name_(name) {
    if (const char* old = std::getenv(name)) old_ = old;
    if (value != nullptr)
      setenv(name, value, 1);
    else
      unsetenv(name);
  }
  ~EnvVar() {
    if (old_)
      setenv(name_, old_->c_str(), 1);
    else
      unsetenv(name_);
  }

 private:
  const char* name_;
  std::optional<std::string> old_;
};

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
  const EnvVar all("LC_ALL", nullptr);
  const EnvVar messages("LC_MESSAGES", nullptr);
  const EnvVar lang("LANG", "de_DE.UTF-8");
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
