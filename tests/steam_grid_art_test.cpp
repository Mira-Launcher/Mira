#include <doctest.h>
#include <stb_image_write.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "steam/GridArt.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

std::string Read(const fs::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

}  // namespace

TEST_CASE("Art picked in Mira goes into Steam's grid, and Steam's own custom art comes back") {
  const fs::path root = test::TempDir("steam-grid");
  const fs::path grid = root / "userdata" / "42" / "config" / "grid";
  fs::create_directories(root / "userdata" / "0" / "config");
  const fs::path files = test::TempDir("steam-grid-files");
  const fs::path written = files / "steam-art.json";
  test::Touch(files / "cover.chosen.png", "mira cover");
  test::Touch(files / "hero.chosen.jpeg", "mira hero");
  test::Touch(files / "logo.chosen.webp", "mira logo");
  // Set in Steam by the user before Mira wrote anything.
  test::Touch(grid / "730p.jpg", "steam cover");
  test::Touch(grid / "730_logo.png", "steam logo");

  REQUIRE(steam::SyncGridArt(root, "730",
                             {{"cover", files / "cover.chosen.png"},
                              {"hero", files / "hero.chosen.jpeg"},
                              {"logo", files / "logo.chosen.webp"}},
                             written));
  CHECK(Read(grid / "730p.png") == "mira cover");
  CHECK_FALSE(fs::exists(grid / "730p.jpg"));  // Steam might show either
  CHECK(Read(grid / "730p.jpg.before-mira") == "steam cover");
  CHECK(Read(grid / "730_hero.jpg") == "mira hero");
  CHECK(Read(grid / "730_logo.png") == "steam logo");  // Steam can't show WebP
  CHECK_FALSE(fs::exists(root / "userdata" / "0" / "config" / "grid"));

  // Another pick replaces Mira's file; the cover's pick taken back restores Steam's.
  test::Touch(files / "hero.chosen.png", "mira hero 2");
  REQUIRE(steam::SyncGridArt(root, "730", {{"hero", files / "hero.chosen.png"}}, written));
  CHECK(Read(grid / "730_hero.png") == "mira hero 2");
  CHECK_FALSE(fs::exists(grid / "730_hero.jpg"));
  CHECK_FALSE(fs::exists(grid / "730p.png"));
  CHECK(Read(grid / "730p.jpg") == "steam cover");

  // Art the user sets in Steam afterwards is theirs, not Mira's to remove.
  test::Touch(grid / "730_icon.png", "steam icon");
  REQUIRE(steam::RemoveGridArt(root, written));
  CHECK_FALSE(fs::exists(grid / "730_hero.png"));
  CHECK(Read(grid / "730_icon.png") == "steam icon");
  CHECK(Read(grid / "730_logo.png") == "steam logo");
}

TEST_CASE("Turning on steam.sync_art puts picks into Steam, and later picks follow") {
  test::LiveServer server(test::TempDir("steam-sync-art"));
  const fs::path steam = server.config().GetString("steam.root");
  fs::create_directories(steam / "steamapps");
  const fs::path grid = steam / "userdata" / "42" / "config" / "grid";
  fs::create_directories(grid.parent_path());
  model::Game game;
  game.id = "steam-620";
  game.source = "steam";
  game.name = "Portal 2";
  game.status = model::GameStatus::Ready;
  REQUIRE(server.games().Upsert(game).has_value());
  httplib::Client client = server.Client();

  // A different shade per image, so each file can be told apart.
  auto upload = [&](const std::string& slot, unsigned char shade) {
    std::vector<unsigned char> pixels(8 * 8 * 4, shade);
    std::string png;
    stbi_write_png_to_func(
        [](void* out, void* data, int size) {
          static_cast<std::string*>(out)->append(static_cast<char*>(data), size);
        },
        &png, 8, 8, 4, pixels.data(), 0);
    REQUIRE(test::AwaitJob(client, client.Put("/v1/games/steam-620/artwork?type=" + slot, png,
                                              "image/png"))["state"] == "finished");
    return png;
  };
  const std::string cover = upload("cover", 10);
  CHECK_FALSE(fs::exists(grid / "620p.png"));  // off by default

  const auto on =
      client.Patch("/v1/config", R"({"steam": {"sync_art": true}})", "application/json");
  REQUIRE(on != nullptr);
  REQUIRE(on->status == 200);
  CHECK(Read(grid / "620p.png") == cover);
  const std::string logo = upload("logo", 20);
  CHECK(Read(grid / "620_logo.png") == logo);

  const auto off =
      client.Patch("/v1/config", R"({"steam": {"sync_art": null}})", "application/json");
  REQUIRE(off != nullptr);
  REQUIRE(off->status == 200);
  CHECK_FALSE(fs::exists(grid / "620p.png"));
  CHECK_FALSE(fs::exists(grid / "620_logo.png"));
}
