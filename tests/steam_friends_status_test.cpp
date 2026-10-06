#include <doctest.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "steam/FriendsStatus.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

// What the stand-in steam was asked, once something arrived.
std::string WaitForCall(const fs::path& log) {
  std::string text;
  for (int tries = 0; tries < 100 && text.empty(); ++tries) {
    std::ifstream file(log);
    std::stringstream buffer;
    buffer << file.rdbuf();
    text = buffer.str();
    if (text.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return text;
}

}  // namespace

TEST_CASE("Setting the Steam status asks a running Steam, and never starts one") {
  const fs::path dir = test::TempDir("steam-friends-status");
  const fs::path log = dir / "calls";
  test::Touch(dir / "bin" / "steam", "#!/bin/sh\necho \"$1\" >> '" + log.string() + "'\n",
              /*executable=*/true);
  const test::PathPrepend path(dir / "bin");
  const fs::path pid_file = dir / "steam.pid";

  SUBCASE("Steam running") {
    test::Touch(pid_file, std::to_string(::getpid()));  // any live pid stands in for Steam
    REQUIRE(steam::SetFriendsStatus("invisible", pid_file));
    CHECK(WaitForCall(log) == "steam://friends/status/invisible\n");
  }
  SUBCASE("Steam not running") {
    const auto set = steam::SetFriendsStatus("online", pid_file);
    REQUIRE_FALSE(set);
    CHECK(set.error().code == "steam_not_running");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_FALSE(fs::exists(log));
  }
  SUBCASE("an unknown status") {
    test::Touch(pid_file, std::to_string(::getpid()));
    const auto set = steam::SetFriendsStatus("away", pid_file);
    REQUIRE_FALSE(set);
    CHECK(set.error().code == "invalid_status");
  }
}
