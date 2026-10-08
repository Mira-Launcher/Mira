#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

#include "config/Config.h"
#include "library/Relocate.h"
#include "lutris/LutrisImporter.h"
#include "runner/Exec.h"
#include "store/GameStore.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
namespace fs = std::filesystem;

namespace {

// Every LutrisRow with runner != 'wine' is inserted too, to exercise the
// skip path, matching a real pga.db (Steam-runner rows sit alongside
// wine-runner ones there).
struct FixtureRow {
  std::string name;
  std::string slug;
  std::string runner;
  std::string configpath;
  std::vector<std::string> categories;  // real Lutris category names, e.g. ".hidden"
  std::int64_t lastplayed = 0;
  double playtime = 0;  // hours, as Lutris keeps it
};

// Schema matches a real pga.db exactly (confirmed via `.schema` against a
// real Lutris install): categories.name is UNIQUE, games_categories is a
// plain join table of game_id/category_id.
Result<void> BuildFixtureDb(const fs::path& db_path, const std::vector<FixtureRow>& rows) {
  std::string sql = "CREATE TABLE games (id INTEGER PRIMARY KEY, name TEXT, slug TEXT, runner TEXT, "
                    "configpath TEXT, lastplayed INTEGER, playtime REAL);"
                    "CREATE TABLE categories (id INTEGER PRIMARY KEY, name TEXT UNIQUE);"
                    "CREATE TABLE games_categories (game_id INTEGER, category_id INTEGER);";
  std::map<std::string, int> category_ids;
  int next_category_id = 1;
  int game_id = 1;
  for (const FixtureRow& row : rows) {
    sql += std::format(
        "INSERT INTO games (id, name, slug, runner, configpath, lastplayed, playtime) "
        "VALUES ({}, '{}', '{}', '{}', '{}', {}, {});",
        game_id, row.name, row.slug, row.runner, row.configpath, row.lastplayed, row.playtime);
    for (const std::string& category : row.categories) {
      const auto [it, inserted] = category_ids.try_emplace(category, next_category_id);
      if (inserted) {
        sql += std::format("INSERT INTO categories (id, name) VALUES ({}, '{}');", next_category_id, category);
        ++next_category_id;
      }
      sql +=
          std::format("INSERT INTO games_categories (game_id, category_id) VALUES ({}, {});", game_id, it->second);
    }
    ++game_id;
  }
  const auto sqlite3 = runner::FindOnPath("sqlite3");
  if (!sqlite3) return Err("sqlite3_missing", "sqlite3 not found");
  Command command;
  command.argv = {*sqlite3, db_path.string(), sql};
  const Result<runner::ExecResult> result = runner::RunAndWait(command);
  if (!result || result->exit_code != 0) {
    return Err("fixture_db_failed", !result ? result.error().message : result->output);
  }
  return {};
}

// The fixtures build pga.db with the sqlite3 tool, which the importer also reads it with.
bool HaveSqlite3() {
  if (runner::FindOnPath("sqlite3")) return true;
  WARN_MESSAGE(false, "skipped: sqlite3 isn't installed");
  return false;
}

struct Fixture {
  fs::path lutris_dir;
  fs::path state_dir;
  config::Config config;
  store::GameStore games;
  api::EventBus events;

  explicit Fixture(const char* name)
      : lutris_dir(TempDir((std::string(name) + "-lutris").c_str())),
        state_dir(TempDir((std::string(name) + "-state").c_str())),
        config(state_dir / "settings.toml"),
        games(state_dir / "mira.db") {
    config.Load();
    test::Isolate(config);
    REQUIRE(config.Set("lutris.data_dir", lutris_dir.string()).has_value());
    games.Load();
    fs::create_directories(lutris_dir / "games");
  }
};

}  // namespace

TEST_CASE("LutrisImporter derives install_path from the exe's own directory, data_dir verbatim from prefix") {
  if (!HaveSqlite3()) return;

  Fixture fx("lutris-combined");
  const fs::path game_dir = fx.lutris_dir.parent_path() / "batman";
  fs::create_directories(game_dir / "Binaries");

  REQUIRE(BuildFixtureDb(fx.lutris_dir / "pga.db",
                        {{"Batman: Arkham Asylum", "batman-arkham-asylum", "wine", "batman-123", {}}})
             .has_value());
  test::Touch(fx.lutris_dir / "games" / "batman-123.yml",
              std::format(R"(game:
  exe: {}/Binaries/BmLauncher.exe
  prefix: {}
)",
                          game_dir.string(), game_dir.string()));

  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  const auto summary = importer.Import();
  REQUIRE(summary.has_value());
  CHECK(summary->added == 1);
  CHECK(summary->other_runner == 0);
  CHECK(summary->incomplete == 0);

  const auto game = fx.games.Find("batman-arkham-asylum");
  REQUIRE(game.has_value());
  CHECK(game->install_path == (game_dir / "Binaries").string());
  CHECK(game->data_dir == game_dir.string());
  CHECK(game->exe_path == "BmLauncher.exe");
  CHECK(game->status == model::GameStatus::Ready);
}

TEST_CASE("LutrisImporter keeps a relative exe relative to prefix, per the yaml itself") {
  if (!HaveSqlite3()) return;

  Fixture fx("lutris-relative-exe");
  const fs::path prefix_dir = fx.lutris_dir.parent_path() / "epic-games-store";
  const fs::path install_dir = prefix_dir / "drive_c" / "Program Files" / "Epic Games" / "Launcher";
  fs::create_directories(install_dir);

  REQUIRE(BuildFixtureDb(fx.lutris_dir / "pga.db", {{"Epic Games Store", "epic-games-store", "wine", "egs-1", {}}})
             .has_value());
  test::Touch(fx.lutris_dir / "games" / "egs-1.yml", std::format(R"(game:
  exe: drive_c/Program Files/Epic Games/Launcher/EpicGamesLauncher.exe
  prefix: {}
)",
                                                                 prefix_dir.string()));

  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  const auto summary = importer.Import();
  REQUIRE(summary.has_value());
  CHECK(summary->added == 1);

  const auto game = fx.games.Find("epic-games-store");
  REQUIRE(game.has_value());
  CHECK(game->install_path == install_dir.string());
  CHECK(game->data_dir == prefix_dir.string());
  CHECK(game->exe_path == "EpicGamesLauncher.exe");
}

TEST_CASE("LutrisImporter skips a row with no prefix recorded in its yaml") {
  if (!HaveSqlite3()) return;

  Fixture fx("lutris-no-prefix");
  REQUIRE(BuildFixtureDb(fx.lutris_dir / "pga.db", {{"Celeste", "celeste", "wine", "celeste-456", {}}}).has_value());
  test::Touch(fx.lutris_dir / "games" / "celeste-456.yml",
              "game:\n  exe: /home/exo/Games/Celeste/Celeste.exe\n");

  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  const auto summary = importer.Import();
  REQUIRE(summary.has_value());
  CHECK(summary->added == 0);
  CHECK(summary->incomplete == 1);
}

TEST_CASE("LutrisImporter skips non-wine runners and updates known games in place") {
  if (!HaveSqlite3()) return;

  Fixture fx("lutris-skip-and-update");
  const fs::path game_dir = fx.lutris_dir.parent_path() / "Celeste";
  fs::create_directories(game_dir);

  REQUIRE(BuildFixtureDb(fx.lutris_dir / "pga.db",
                        {{"Celeste", "celeste", "wine", "celeste-1", {}},
                         {"Half-Life", "half-life", "steam", "half-life-1", {}}})
             .has_value());
  test::Touch(fx.lutris_dir / "games" / "celeste-1.yml",
              std::format(R"(game:
  exe: {}/Celeste.exe
  prefix: {}
)",
                          game_dir.string(), game_dir.string()));
  // The steam-runner row has no yaml fixture at all, so ReadGameConfig fails
  // to open it, which is exactly what "not a wine game we handle" looks
  // like in a real pga.db too (Steam rows aren't given a Lutris yaml).

  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  const auto first = importer.Import();
  REQUIRE(first.has_value());
  CHECK(first->added == 1);
  CHECK(first->other_runner == 1);

  const auto second = importer.Import();
  REQUIRE(second.has_value());
  CHECK(second->added == 0);
  CHECK(second->updated == 1);
  CHECK(fx.games.All().size() == 1);
}

TEST_CASE("LutrisImporter keeps a game Mira moved into its own folders on re-import") {
  if (!HaveSqlite3()) return;

  Fixture fx("lutris-moved");
  const fs::path library = fx.state_dir / "library";
  REQUIRE(fx.config.Set("library_roots", nlohmann::json::array({library.string()})).has_value());
  const fs::path game_dir = fx.lutris_dir.parent_path() / "lutris-moved-game" / "Blue Prince";
  const fs::path prefix_dir = fx.lutris_dir.parent_path() / "lutris-moved-prefix";
  test::Touch(game_dir / "BLUE PRINCE.exe", "exe");
  fs::create_directories(prefix_dir / "drive_c");
  REQUIRE(BuildFixtureDb(fx.lutris_dir / "pga.db", {{"Blue Prince", "blue-prince", "wine", "blue-prince-1", {}}})
              .has_value());
  test::Touch(fx.lutris_dir / "games" / "blue-prince-1.yml",
              std::format("game:\n  exe: {}/BLUE PRINCE.exe\n  prefix: {}\n", game_dir.string(), prefix_dir.string()));

  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  REQUIRE(importer.Import().has_value());
  const auto imported = fx.games.Find("blue-prince");
  REQUIRE(imported.has_value());
  const auto moved = library::Relocate(fx.config, *imported, {}, fx.games.All());
  REQUIRE(moved.has_value());
  REQUIRE(fx.games.Upsert(*moved).has_value());

  const auto again = importer.Import();
  REQUIRE(again.has_value());
  CHECK(again->added == 0);
  CHECK(fx.games.All().size() == 1);
  const auto kept = fx.games.Find("blue-prince");
  REQUIRE(kept.has_value());
  CHECK(kept->install_path == moved->install_path);
  CHECK(kept->data_dir == moved->data_dir);
  CHECK(fs::exists(fs::path(kept->install_path) / kept->exe_path));
  CHECK(kept->source == "manual");  // Lutris can't run it from there, so it's Mira's own
}

TEST_CASE("LutrisImporter brings Lutris's playtime and last played until Mira has played the game itself") {
  if (!HaveSqlite3()) return;

  Fixture fx("lutris-play");
  const fs::path game_dir = fx.lutris_dir.parent_path() / "lutris-play-game";
  fs::create_directories(game_dir);
  const auto build = [&](std::int64_t lastplayed, double hours) {
    fs::remove(fx.lutris_dir / "pga.db");
    REQUIRE(BuildFixtureDb(fx.lutris_dir / "pga.db",
                           {{"Celeste", "celeste", "wine", "celeste-1", {}, lastplayed, hours}})
                .has_value());
  };
  test::Touch(fx.lutris_dir / "games" / "celeste-1.yml",
              std::format("game:\n  exe: {}/Celeste.exe\n  prefix: {}\n", game_dir.string(), game_dir.string()));
  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);

  build(1780112448, 118.5);
  REQUIRE(importer.Import().has_value());
  CHECK(fx.games.Find("celeste")->last_played_at == 1780112448);
  CHECK(fx.games.Find("celeste")->play_seconds == 118 * 3600 + 1800);

  build(1790000000, 120);  // played more in Lutris
  REQUIRE(importer.Import().has_value());
  CHECK(fx.games.Find("celeste")->play_seconds == 120 * 3600);

  // Once Mira has a session of its own, its record stays.
  REQUIRE(fx.games.Update("celeste", [](model::Game& g) {
    g.last_session_at = 1791000000;
    g.last_played_at = 1791000000;
    g.play_seconds = 5;
  }).has_value());
  build(1792000000, 200);
  REQUIRE(importer.Import().has_value());
  CHECK(fx.games.Find("celeste")->last_played_at == 1791000000);
  CHECK(fx.games.Find("celeste")->play_seconds == 5);
}

TEST_CASE("LutrisImporter offers the folder's executables and keeps the one picked in Mira") {
  if (!HaveSqlite3()) return;

  Fixture fx("lutris-pick");
  const fs::path game_dir = fx.lutris_dir.parent_path() / "lutris-pick-game";
  test::Touch(game_dir / "Launcher.exe", std::string(4096, 'x'));
  test::Touch(game_dir / "bin" / "Game.exe", std::string(8192, 'x'));
  REQUIRE(BuildFixtureDb(fx.lutris_dir / "pga.db", {{"Hades", "hades", "wine", "hades-1", {}}}).has_value());
  test::Touch(fx.lutris_dir / "games" / "hades-1.yml",
              std::format("game:\n  exe: {}/Launcher.exe\n  prefix: {}\n", game_dir.string(), game_dir.string()));
  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  REQUIRE(importer.Import().has_value());

  const auto imported = fx.games.Find("hades");
  REQUIRE(imported.has_value());
  CHECK(std::ranges::any_of(imported->candidates, [](const model::Candidate& c) { return c.rel_path == "bin/Game.exe"; }));
  CHECK(std::ranges::any_of(imported->candidates,
                            [](const model::Candidate& c) { return c.rel_path == "Launcher.exe" && c.chosen; }));

  REQUIRE(fx.games.Update("hades", [](model::Game& g) { g.exe_path = "bin/Game.exe"; }).has_value());
  REQUIRE(importer.Import().has_value());
  CHECK(fx.games.Find("hades")->exe_path == "bin/Game.exe");
  CHECK(fx.games.Find("hades")->source == "lutris");
}

TEST_CASE("LutrisImporter takes over a game an older Mira moved but left as a Lutris game") {
  if (!HaveSqlite3()) return;

  Fixture fx("lutris-old-move");
  const fs::path game_dir = fx.lutris_dir.parent_path() / "lutris-old-move-game";
  const fs::path prefix_dir = fx.lutris_dir.parent_path() / "lutris-old-move-prefix";
  test::Touch(game_dir / "Game.exe", "exe");
  fs::create_directories(prefix_dir / "drive_c");
  REQUIRE(BuildFixtureDb(fx.lutris_dir / "pga.db", {{"Jump King", "jump-king", "wine", "jump-king-1", {}}}).has_value());
  test::Touch(fx.lutris_dir / "games" / "jump-king-1.yml",
              std::format("game:\n  exe: {}/Game.exe\n  prefix: {}\n", game_dir.string(), prefix_dir.string()));
  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  REQUIRE(importer.Import().has_value());

  // Only the prefix moved, the way the old move left Lutris games.
  const fs::path moved_prefix = fx.state_dir / "prefixes" / "jump-king";
  fs::create_directories(moved_prefix.parent_path());
  fs::rename(prefix_dir, moved_prefix);
  REQUIRE(fx.games.Update("jump-king", [&](model::Game& g) { g.data_dir = moved_prefix.string(); }).has_value());

  REQUIRE(importer.Import().has_value());
  const auto taken = fx.games.Find("jump-king");
  REQUIRE(taken.has_value());
  CHECK(taken->source == "manual");
  CHECK(taken->data_dir == moved_prefix.string());
  REQUIRE(importer.Import().has_value());
  CHECK(fx.games.All().size() == 1);
}

TEST_CASE("LutrisImporter refuses an install_path that's really the whole shared prefix") {
  if (!HaveSqlite3()) return;

  Fixture fx("lutris-broad-install-path");
  const fs::path prefix_dir = fx.lutris_dir.parent_path() / "battlenet";
  fs::create_directories(prefix_dir / "drive_c");

  REQUIRE(BuildFixtureDb(fx.lutris_dir / "pga.db", {{"Hearthstone", "hearthstone", "wine", "hs-1", {}}}).has_value());
  // A launcher script referenced with no subdirectory at all: install_path
  // would resolve to prefix/drive_c, the whole C: drive shared by every
  // other game in this prefix (Battle.net, HDT, ...). Must be refused, not
  // handed out as a deletion scope.
  test::Touch(fx.lutris_dir / "games" / "hs-1.yml", std::format(R"(game:
  exe: drive_c/launch-hdt.bat
  prefix: {}
)",
                                                                prefix_dir.string()));

  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  const auto summary = importer.Import();
  REQUIRE(summary.has_value());
  CHECK(summary->added == 0);
  CHECK(summary->incomplete == 1);
  CHECK(fx.games.All().empty());
}

TEST_CASE("LutrisImporter maps .hidden to the hidden tag, favorites to favorite, and everything else verbatim") {
  if (!HaveSqlite3()) return;

  Fixture fx("lutris-categories");
  const fs::path game_dir = fx.lutris_dir.parent_path() / "Celeste";
  fs::create_directories(game_dir);

  REQUIRE(BuildFixtureDb(fx.lutris_dir / "pga.db",
                        {{"Celeste", "celeste", "wine", "celeste-1", {".hidden", "favorites", "Platformer"}}})
             .has_value());
  test::Touch(fx.lutris_dir / "games" / "celeste-1.yml",
              std::format(R"(game:
  exe: {}/Celeste.exe
  prefix: {}
)",
                          game_dir.string(), game_dir.string()));

  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  const auto summary = importer.Import();
  REQUIRE(summary.has_value());
  CHECK(summary->added == 1);

  const auto game = fx.games.Find("celeste");
  REQUIRE(game.has_value());
  CHECK(std::ranges::find(game->tags, "hidden") != game->tags.end());
  CHECK(std::ranges::find(game->tags, "favorite") != game->tags.end());
  CHECK(std::ranges::find(game->tags, "Platformer") != game->tags.end());
}

TEST_CASE("LutrisImporter merges Lutris categories with tags the user already added, on re-import") {
  if (!HaveSqlite3()) return;

  Fixture fx("lutris-tag-merge");
  const fs::path game_dir = fx.lutris_dir.parent_path() / "Celeste";
  fs::create_directories(game_dir);

  REQUIRE(
      BuildFixtureDb(fx.lutris_dir / "pga.db", {{"Celeste", "celeste", "wine", "celeste-1", {".hidden"}}})
          .has_value());
  test::Touch(fx.lutris_dir / "games" / "celeste-1.yml",
              std::format(R"(game:
  exe: {}/Celeste.exe
  prefix: {}
)",
                          game_dir.string(), game_dir.string()));

  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  REQUIRE(importer.Import().has_value());

  auto stored = fx.games.Find("celeste");
  REQUIRE(stored.has_value());
  stored->tags.push_back("my-own-tag");
  REQUIRE(fx.games.Upsert(*stored).has_value());

  REQUIRE(importer.Import().has_value());

  const auto game = fx.games.Find("celeste");
  REQUIRE(game.has_value());
  CHECK(std::ranges::find(game->tags, "hidden") != game->tags.end());
  CHECK(std::ranges::find(game->tags, "my-own-tag") != game->tags.end());
}

TEST_CASE("LutrisImporter imports a native (\"linux\" runner) game with no prefix at all") {
  if (!HaveSqlite3()) return;

  Fixture fx("lutris-native");
  const fs::path game_dir = fx.lutris_dir.parent_path() / "MyAppImageGame";
  fs::create_directories(game_dir);

  REQUIRE(BuildFixtureDb(fx.lutris_dir / "pga.db",
                        {{"My AppImage Game", "my-appimage-game", "linux", "native-1", {}}})
             .has_value());
  test::Touch(fx.lutris_dir / "games" / "native-1.yml", std::format(R"(game:
  exe: {}/MyGame.AppImage
  args: --fullscreen
)",
                                                                    game_dir.string()));

  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  const auto summary = importer.Import();
  REQUIRE(summary.has_value());
  CHECK(summary->added == 1);
  CHECK(summary->other_runner == 0);
  CHECK(summary->incomplete == 0);

  const auto game = fx.games.Find("my-appimage-game");
  REQUIRE(game.has_value());
  CHECK(game->platform == model::Platform::Native);
  CHECK(game->install_path == game_dir.string());
  CHECK(game->exe_path == "MyGame.AppImage");
  CHECK(game->args == "--fullscreen");
  CHECK(game->data_dir.empty());
  CHECK(game->status == model::GameStatus::Ready);
}

TEST_CASE("LutrisImporter skips a \"linux\" row whose exe is relative -- nothing to resolve it against") {
  if (!HaveSqlite3()) return;

  Fixture fx("lutris-native-relative-exe");
  REQUIRE(
      BuildFixtureDb(fx.lutris_dir / "pga.db", {{"Broken", "broken", "linux", "native-2", {}}}).has_value());
  test::Touch(fx.lutris_dir / "games" / "native-2.yml", "game:\n  exe: MyGame.AppImage\n");

  lutris::LutrisImporter importer(fx.config, fx.games, fx.events);
  const auto summary = importer.Import();
  REQUIRE(summary.has_value());
  CHECK(summary->added == 0);
  CHECK(summary->incomplete == 1);
}
