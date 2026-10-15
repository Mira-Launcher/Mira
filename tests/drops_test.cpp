#include "library/Drops.h"

#include <doctest.h>

#include <filesystem>

#include "emulators/Emulators.h"
#include "support/TestEnv.h"

namespace fs = std::filesystem;
using namespace mira;

TEST_CASE("drops: a ROM goes to the one emulator that runs it, and Mira asks when several do") {
  test::TestEnv env("drops");
  const fs::path mira = env.dir / "Mira";
  REQUIRE(env.config.Set("mira_folders", nlohmann::json::array({mira.string()})));
  REQUIRE(env.config.Set(
      "library_roots",
      nlohmann::json::array({(mira / "Games").string(), (mira / "Applications").string()})));
  REQUIRE(env.config.Set("emulators.roms_root", (mira / "Roms").string()));
  for (const char* id : {"snes9x", "mesen"}) {
    test::Touch(env.dir / "bin" / id, "", true);
    REQUIRE(
        env.config.Set(std::string("emulators.") + id + ".bin", (env.dir / "bin" / id).string()));
  }
  env.games.SaveSource({.id = "snes9x", .added = true});

  CHECK(library::IsMiraFoldersOwn(env.config, mira / "Games"));
  CHECK(library::IsMiraFoldersOwn(env.config, mira / "Roms"));
  CHECK(library::IsMiraFoldersOwn(env.config, mira / ".mira-import"));
  CHECK_FALSE(library::IsMiraFoldersOwn(env.config, mira / "Earthbound (USA).sfc"));

  library::Drops drops(env.config, env.games, env.events);
  const fs::path rom_dir = emulators::RomDir(env.config, *emulators::Find("snes9x"));
  test::Touch(mira / "Earthbound (USA).sfc");
  drops.Sort(mira / "Earthbound (USA).sfc");
  CHECK_FALSE(fs::exists(mira / "Earthbound (USA).sfc"));
  CHECK(fs::exists(rom_dir / "Earthbound (USA).sfc"));

  // A homebrew's EBOOT.PBP would take the folder around it: the Mira folder itself.
  env.games.SaveSource({.id = "ppsspp", .added = true});
  test::Touch(mira / "EBOOT.PBP");
  drops.Sort(mira / "EBOOT.PBP");
  CHECK(fs::exists(mira / "EBOOT.PBP"));
  CHECK(fs::exists(mira / "readme.txt") == false);

  // Already there when mirad started: asked about, never moved.
  test::Touch(mira / "Earthbound (Japan).sfc");
  drops.Sort(mira / "Earthbound (Japan).sfc", /*ask=*/true);
  CHECK(fs::exists(mira / "Earthbound (Japan).sfc"));
  REQUIRE(drops.All().size() == 1);
  CHECK(drops.All()[0].choices == std::vector<std::string>{"snes9x"});
  fs::remove(mira / "Earthbound (Japan).sfc");
  drops.Forget(mira / "Earthbound (Japan).sfc");

  test::Touch(mira / "readme.txt");
  drops.Sort(mira / "readme.txt");
  CHECK(fs::exists(mira / "readme.txt"));
  CHECK(drops.All().empty());

  env.games.SaveSource({.id = "mesen", .added = true});
  test::Touch(mira / "Super Metroid (USA).smc");
  drops.Sort(mira / "Super Metroid (USA).smc");
  REQUIRE(drops.All().size() == 1);
  CHECK(drops.All()[0].choices == std::vector<std::string>{"mesen", "snes9x"});
  CHECK_FALSE(drops.Settle(mira / "Super Metroid (USA).smc", "game"));
  REQUIRE(drops.Settle(mira / "Super Metroid (USA).smc", "snes9x"));
  CHECK(fs::exists(rom_dir / "Super Metroid (USA).smc"));
  CHECK(drops.All().empty());
}
