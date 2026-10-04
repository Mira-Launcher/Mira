#include <doctest.h>

#include <filesystem>

#include "config/Config.h"
#include "library/PrefixNaming.h"
#include "library/Relocate.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

fs::path TempDir(const char* name) {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

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
