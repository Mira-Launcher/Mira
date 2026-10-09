#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "steam/Shortcuts.h"
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

TEST_CASE("The Mira shortcut is added to Steam once, keeping the user's own shortcuts") {
  const fs::path root = test::TempDir("steam-shortcuts");
  const fs::path file = root / "userdata" / "42" / "config" / "shortcuts.vdf";
  fs::create_directories(file.parent_path());
  // One existing shortcut: AppName "Game", appid 7, an empty tags object.
  using namespace std::string_literals;
  const std::string game = "\x00" "0\x00" "\x01" "AppName\x00" "Game\x00" "\x02" "appid\x00" "\x07\x00\x00\x00"
                           "\x00" "tags\x00" "\x08" "\x08"s;
  test::Touch(file, "\x00" "shortcuts\x00"s + game + "\x08\x08"s);

  auto change = steam::EnsureShortcut(root, "Mira", "/opt/Mira.AppImage", "--big-screen");
  REQUIRE(change);
  CHECK(change->added == std::vector<std::string>{"42"});
  const std::string written = Read(file);
  CHECK(written.find(game) != std::string::npos);
  CHECK(written.find("\x01" "Exe\x00" "\"/opt/Mira.AppImage\"\x00"s) != std::string::npos);
  CHECK(written.find("\x01" "LaunchOptions\x00" "--big-screen\x00"s) != std::string::npos);

  change = steam::EnsureShortcut(root, "Mira", "/opt/Mira.AppImage", "--big-screen");
  REQUIRE(change);
  CHECK(change->added.empty());
  CHECK(change->updated.empty());
  CHECK(Read(file) == written);

  change = steam::EnsureShortcut(root, "Mira", "/home/a/Mira.AppImage", "--big-screen");
  REQUIRE(change);
  CHECK(change->updated == std::vector<std::string>{"42"});
}
