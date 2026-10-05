#include <doctest.h>

#include <filesystem>
#include <format>

#include "steam/SteamScanner.h"
#include "support/TestEnv.h"

using namespace mira;
using test::Touch;
namespace fs = std::filesystem;

namespace {

// SteamID64s and the userdata/ folder each maps to.
constexpr const char* kRecentUser = "76561198000000001";
constexpr const char* kOtherUser = "76561198000000002";
constexpr const char* kRecentFolder = "39734273";
constexpr const char* kOtherFolder = "39734274";

void InstallApp(const fs::path& steam, const std::string& appid, const std::string& name) {
  Touch(steam / "steamapps" / std::format("appmanifest_{}.acf", appid),
        std::format(R"("AppState" {{ "appid" "{0}" "name" "{1}" "installdir" "{1}" }})", appid, name));
  Touch(steam / "steamapps" / "common" / name / "game", "", /*executable=*/true);
}

void WriteActivity(const fs::path& steam, const char* folder, const std::string& apps) {
  Touch(steam / "userdata" / folder / "config" / "localconfig.vdf",
        std::format(R"("UserLocalConfigStore" {{ "Software" {{ "Valve" {{ "Steam" {{ "apps" {{ {} }} }} }} }} }})",
                    apps));
}

}  // namespace

TEST_CASE("A Steam scan takes the later last played and the larger playtime from Steam's own records") {
  test::TestEnv env("steam-activity");
  REQUIRE(env.config.Set("steam.import_playtime", true).has_value());
  const fs::path steam = env.config.GetPath("steam.root");
  InstallApp(steam, "400", "Portal");
  InstallApp(steam, "620", "Portal 2");
  Touch(steam / "config" / "loginusers.vdf",
        std::format(R"("users" {{ "{}" {{ "MostRecent" "1" "Timestamp" "100" }} "{}" {{ "Timestamp" "200" }} }})",
                    kRecentUser, kOtherUser));
  WriteActivity(steam, kRecentFolder,
                R"("400" { "LastPlayed" "1700000000" "Playtime" "90" } "620" { "LastPlayed" "1600000000" })");
  WriteActivity(steam, kOtherFolder, R"("400" { "LastPlayed" "1800000000" "Playtime" "9000" })");

  // Played later in Mira than Steam remembers.
  model::Game portal2;
  portal2.id = "steam-620";
  portal2.source = "steam";
  portal2.name = "Portal 2";
  portal2.last_played_at = 1650000000;
  portal2.play_seconds = 600;
  REQUIRE(env.games.Upsert(portal2).has_value());

  steam::SteamScanner scanner(env.config, env.games, env.events);
  REQUIRE(scanner.Scan().has_value());

  const auto portal = env.games.Find("steam-400");
  REQUIRE(portal.has_value());
  CHECK(portal->last_played_at == 1700000000);  // the signed-in account's, not the other one's
  CHECK(portal->play_seconds == 90 * 60);
  const auto kept = env.games.Find("steam-620");
  REQUIRE(kept.has_value());
  CHECK(kept->last_played_at == 1650000000);
  CHECK(kept->play_seconds == 600);

  SUBCASE("the configured Steam ID picks the account") {
    REQUIRE(env.config.Set("steam.steamid64", kOtherUser).has_value());
    REQUIRE(scanner.Scan().has_value());
    CHECK(env.games.Find("steam-400")->last_played_at == 1800000000);
  }

  SUBCASE("turning the import off leaves Mira's own record alone") {
    REQUIRE(env.config.Set("steam.import_playtime", false).has_value());
    InstallApp(steam, "70", "Half-Life");
    WriteActivity(steam, kRecentFolder, R"("70" { "LastPlayed" "1700000000" "Playtime" "5" })");
    REQUIRE(scanner.Scan().has_value());
    CHECK_FALSE(env.games.Find("steam-70")->last_played_at.has_value());
  }
}
