#include <doctest.h>

#include <filesystem>

#include "config/Config.h"
#include "saves/remote/Rclone.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

TEST_CASE("rclone counts from 1.76 and is found from the setting, then PATH, when new enough") {
  CHECK(saves::remote::NewEnough("rclone v1.76.0\n- os/version: linux"));
  CHECK(saves::remote::NewEnough("v1.77.2"));
  CHECK(saves::remote::NewEnough("v2.0.0"));
  CHECK_FALSE(saves::remote::NewEnough("v1.60.1-DEV"));
  CHECK_FALSE(saves::remote::NewEnough("v1.75.9"));
  CHECK_FALSE(saves::remote::NewEnough(""));
  CHECK_FALSE(saves::remote::NewEnough("rclone unknown"));

  const fs::path dir = test::TempDir("remote-saves-rclone");
  config::Config config(dir / "settings.toml");
  config.Load();
  test::Isolate(config);

  const fs::path fresh = dir / "fresh" / "rclone";
  test::Touch(fresh, "#!/bin/sh\necho 'rclone v1.76.0'\n", true);
  REQUIRE(config.Set("saves.rclone_bin", fresh.string()));
  const runner::ToolStatus found = saves::remote::DetectRclone(config);
  CHECK(found.installed);
  CHECK(found.source == "override");

  const fs::path old = dir / "old" / "rclone";
  test::Touch(old, "#!/bin/sh\necho 'rclone v1.60.1-DEV'\n", true);
  REQUIRE(config.Set("saves.rclone_bin", old.string()));
  CHECK_FALSE(saves::remote::DetectRclone(config).installed);

  REQUIRE(config.Set("saves.rclone_bin", ""));
  const test::PathPrepend path(dir / "old");
  CHECK_FALSE(saves::remote::DetectRclone(config).installed);
}
