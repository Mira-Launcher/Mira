#include <doctest.h>

#include <filesystem>

#include "config/Config.h"
#include "core/Paths.h"
#include "library/PrefixNaming.h"
#include "library/Relocate.h"
#include "support/TestEnv.h"

using namespace mira;
using test::TempDir;
namespace fs = std::filesystem;

namespace {

fs::path TempConfigFile(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests";
  fs::create_directories(dir);
  const fs::path file = dir / name;
  fs::remove(file);
  return file;
}

}  // namespace

TEST_CASE("PrefixDir names by slugified game name by default") {
  config::Config config(TempConfigFile("prefix-naming-default.toml"));
  config.Load();
  REQUIRE(config.Set("prefix_root", TempDir("prefix-naming-root").string()).has_value());

  model::Game game;
  game.id = "gog-1207660413";
  game.name = "Celeste";
  CHECK(library::PrefixDir(config, game).filename() == "celeste");
}

TEST_CASE("PrefixDir uses the raw id when prefix_naming is \"id\"") {
  config::Config config(TempConfigFile("prefix-naming-id.toml"));
  config.Load();
  const fs::path root = TempDir("prefix-naming-id-root");
  REQUIRE(config.Set("prefix_root", root.string()).has_value());
  REQUIRE(config.Set("prefix_naming", "id").has_value());

  model::Game game;
  game.id = "gog-1207660413";
  game.name = "Celeste";
  CHECK(library::PrefixDir(config, game) == root / "gog-1207660413");
}

TEST_CASE("PrefixDir appends a numeric suffix on a real directory collision") {
  config::Config config(TempConfigFile("prefix-naming-collision.toml"));
  config.Load();
  const fs::path root = TempDir("prefix-naming-collision-root");
  REQUIRE(config.Set("prefix_root", root.string()).has_value());
  fs::create_directories(root / "celeste");

  model::Game game;
  game.id = "gog-1207660413";
  game.name = "Celeste";
  CHECK(library::PrefixDir(config, game).filename() == "celeste-2");
}

TEST_CASE("PrefixDir skips a folder another game already owns even though it isn't created yet") {
  config::Config config(TempConfigFile("prefix-naming-owned.toml"));
  config.Load();
  const fs::path root = TempDir("prefix-naming-owned-root");
  REQUIRE(config.Set("prefix_root", root.string()).has_value());
  store::GameStore games(root / "games.toml");
  games.Load();
  model::Game first;
  first.id = "a";
  first.name = "Celeste";
  first.data_dir = (root / "celeste").string();  // not provisioned: nothing on disk
  REQUIRE(games.Upsert(first).has_value());

  model::Game second;
  second.id = "b";
  second.name = "Celeste";
  CHECK(library::PrefixDir(config, games, second).filename() == "celeste-2");
  CHECK(library::PrefixDir(config, games, first).filename() == "celeste");  // its own, not a collision
}

TEST_CASE("PrefixDir falls back to strings::Slugify's own \"game\" default for a name with no letters or digits") {
  config::Config config(TempConfigFile("prefix-naming-empty.toml"));
  config.Load();
  const fs::path root = TempDir("prefix-naming-empty-root");
  REQUIRE(config.Set("prefix_root", root.string()).has_value());

  model::Game game;
  game.id = "itch-42";
  game.name = "!!!";
  CHECK(library::PrefixDir(config, game) == root / "game");
}

TEST_CASE("Relocate leaves a prefix already at its named directory in place") {
  config::Config config(TempConfigFile("prefix-naming-relocate.toml"));
  config.Load();
  const fs::path root = TempDir("prefix-naming-relocate-root");
  REQUIRE(config.Set("prefix_root", root.string()).has_value());
  fs::create_directories(root / "celeste" / "drive_c");

  model::Game game;
  game.id = "gog-1207660413";
  game.name = "Celeste";
  game.data_dir = (root / "celeste").string();
  const auto relocated = library::Relocate(config, game);
  REQUIRE(relocated.has_value());
  CHECK(relocated->data_dir == game.data_dir);
  CHECK(fs::is_directory(root / "celeste" / "drive_c"));
}

TEST_CASE("Relocate with only an install target moves the files and leaves the prefix where it is") {
  config::Config config(TempConfigFile("relocate-only-given.toml"));
  config.Load();
  const fs::path library = TempDir("relocate-only-given-lib");
  const fs::path elsewhere = TempDir("relocate-only-given-elsewhere");
  REQUIRE(config.Set("library_roots", nlohmann::json::array({library.string()})).has_value());
  REQUIRE(config.Set("prefix_root", TempDir("relocate-only-given-prefixes").string()).has_value());
  fs::create_directories(library / "old" / "Celeste");
  fs::create_directories(elsewhere / "pfx" / "drive_c");

  model::Game game;
  game.id = "celeste";
  game.name = "Celeste";
  game.install_path = (library / "old" / "Celeste").string();
  game.data_dir = (elsewhere / "pfx").string();  // outside prefix_root
  library::RelocateRequest request;
  request.install_path = library / "new" / "Celeste";
  request.only_given = true;
  const auto relocated = library::Relocate(config, game, request);
  REQUIRE(relocated.has_value());
  CHECK(relocated->install_path == (library / "new" / "Celeste").string());
  CHECK(fs::is_directory(library / "new" / "Celeste"));
  CHECK(relocated->data_dir == game.data_dir);
  CHECK(fs::is_directory(elsewhere / "pfx" / "drive_c"));
}

TEST_CASE("NeedsProvisioning retries a broken store game") {
  model::Game game;
  game.runner_ref = "proton:GE-Proton9-20";
  game.data_dir = "/prefixes/celeste";
  game.status = model::GameStatus::Ready;
  CHECK_FALSE(library::NeedsProvisioning(game));
  game.status = model::GameStatus::Broken;
  CHECK(library::NeedsProvisioning(game));
  CHECK(library::NeedsProvisioning(std::nullopt));
}

namespace {

struct RelocateEnv {
  test::TestEnv env{"relocate-lutris"};
  fs::path library = env.dir / "library";
  fs::path prefixes = env.config.GetPath("prefix_root");
  fs::path outside = env.dir / "lutris-games";

  RelocateEnv() { REQUIRE(env.config.Set("library_roots", nlohmann::json::array({library.string()})).has_value()); }

  model::Game LutrisGame(const std::string& id, const std::string& name, const fs::path& install, const fs::path& prefix) {
    test::Touch(install / "Game.exe", "exe");
    fs::create_directories(prefix / "drive_c");
    model::Game game;
    game.id = id;
    game.name = name;
    game.source = "lutris";
    game.install_path = install.string();
    game.exe_path = "Game.exe";
    game.data_dir = prefix.string();
    return game;
  }
};

}  // namespace

TEST_CASE("Relocate moves a Lutris game's files as well as its prefix") {
  RelocateEnv setup;
  const model::Game game = setup.LutrisGame("blue-prince", "Blue Prince", setup.outside / "blue-prince",
                                            setup.outside / "prefixes" / "blue-prince");
  const auto relocated = library::Relocate(setup.env.config, game, {}, std::vector{game});
  REQUIRE(relocated.has_value());
  CHECK(paths::IsWithin(relocated->install_path, {setup.library}));
  CHECK(fs::exists(fs::path(relocated->install_path) / "Game.exe"));
  CHECK(paths::IsWithin(relocated->data_dir, {setup.prefixes}));
  CHECK(fs::is_directory(fs::path(relocated->data_dir) / "drive_c"));
  CHECK_FALSE(fs::exists(game.install_path));
}

TEST_CASE("Relocate moves a prefix that holds the game as one folder") {
  RelocateEnv setup;
  const fs::path prefix = setup.outside / "nine-sols";
  const model::Game game =
      setup.LutrisGame("nine-sols", "Nine Sols", prefix / "drive_c" / "Games" / "Nine Sols", prefix);
  const auto relocated = library::Relocate(setup.env.config, game, {}, std::vector{game});
  REQUIRE(relocated.has_value());
  REQUIRE(paths::IsWithin(relocated->data_dir, {setup.prefixes}));
  CHECK(relocated->install_path == (fs::path(relocated->data_dir) / "drive_c" / "Games" / "Nine Sols").string());
  CHECK(fs::exists(fs::path(relocated->install_path) / "Game.exe"));
  CHECK_FALSE(fs::exists(prefix));

  SUBCASE("and the same when the install folder is the prefix itself") {
    const fs::path combined = setup.outside / "cuphead";
    const model::Game cuphead = setup.LutrisGame("cuphead", "Cuphead", combined, combined);
    const auto moved = library::Relocate(setup.env.config, cuphead, {}, std::vector{cuphead});
    REQUIRE(moved.has_value());
    CHECK(moved->install_path == moved->data_dir);
    CHECK(fs::exists(fs::path(moved->install_path) / "Game.exe"));
  }
}

TEST_CASE("Relocate refuses to move an install folder that holds other games") {
  RelocateEnv setup;
  const fs::path shared = setup.outside / "Installed-Games";
  model::Game loose = setup.LutrisGame("osu", "osu!", shared, setup.outside / "unused");
  loose.data_dir.clear();
  const model::Game neighbour =
      setup.LutrisGame("jump-king", "Jump King", shared / "jump-king", shared / "jump-king");
  const auto relocated = library::Relocate(setup.env.config, loose, {}, std::vector{loose, neighbour});
  REQUIRE_FALSE(relocated.has_value());
  CHECK(relocated.error().code == "shared_folder");
  CHECK(fs::exists(shared / "Game.exe"));
  CHECK(fs::exists(shared / "jump-king" / "Game.exe"));
}
