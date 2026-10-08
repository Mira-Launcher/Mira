#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "setup/Setup.h"

using namespace mira;
namespace fs = std::filesystem;

TEST_CASE("setup::Install writes the wrapper, links, desktop entry and unit; Remove undoes it and drops the games' entries") {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / "setup";
  fs::remove_all(dir);
  fs::create_directories(dir);
  std::ofstream(dir / "Mira.AppImage") << "fake";
  std::ofstream(dir / "mira.png") << "png";
  const setup::SetupPaths paths{dir / "bin", dir / "applications", dir / "icons", dir / "systemd"};

  const auto written = setup::Install(paths, dir / "Mira.AppImage", dir / "mira.png");
  REQUIRE(written);
  CHECK(fs::read_symlink(paths.bin_dir / "mirad") == "mira");
  std::ifstream unit(paths.systemd_dir / "mirad.service");
  const std::string unit_text((std::istreambuf_iterator<char>(unit)), std::istreambuf_iterator<char>());
  CHECK(unit_text.find("ExecStart=" + (paths.bin_dir / "mirad").string()) != std::string::npos);
  CHECK(fs::exists(paths.icons_dir / "hicolor/256x256/apps/mira.png"));

  // A game's menu entry runs the wrapper; another app's entry is left alone.
  std::ofstream(paths.applications_dir / "mira-balatro.desktop") << "[Desktop Entry]\nX-Mira-Game-Id=balatro\n";
  std::ofstream(paths.applications_dir / "mira-other.desktop") << "[Desktop Entry]\nName=Other\n";

  REQUIRE(setup::Remove(paths));
  CHECK_FALSE(fs::exists(paths.applications_dir / "mira-balatro.desktop"));
  CHECK(fs::exists(paths.applications_dir / "mira-other.desktop"));
  CHECK_FALSE(fs::exists(paths.bin_dir / "mira"));
  CHECK_FALSE(fs::exists(fs::symlink_status(paths.bin_dir / "mirad")));
  CHECK_FALSE(fs::exists(paths.applications_dir / "mira.desktop"));
}

TEST_CASE("setup::Install refuses to replace a mira it didn't write") {
  const fs::path dir = fs::temp_directory_path() / "mira-tests" / "setup-conflict";
  fs::remove_all(dir);
  fs::create_directories(dir / "bin");
  std::ofstream(dir / "Mira.AppImage") << "fake";
  std::ofstream(dir / "bin" / "mira") << "#!/bin/sh\necho someone else's\n";
  const setup::SetupPaths paths{dir / "bin", dir / "applications", dir / "icons", dir / "systemd"};

  const auto written = setup::Install(paths, dir / "Mira.AppImage", dir / "none.png");
  REQUIRE_FALSE(written);
  CHECK(written.error().code == "setup_conflict");
}
