#include <doctest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <sstream>

#include "proc/Session.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string Read(const fs::path& path) {
  std::ifstream file(path);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

}  // namespace

TEST_CASE("Ready-made launch steps change the computer only while the game runs") {
  const fs::path dir = test::TempDir("script-steps");
  const fs::path state = dir / "state";
  // Stand-ins that keep the state the real tools would change: the Steam status and the power
  // profile.
  test::Touch(dir / "bin" / "steam",
              "#!/bin/sh\necho \"${1##*/}\" > '" + (state / "steam").string() + "'\n",
              /*executable=*/true);
  const std::string profile = (state / "profile").string();
  test::Touch(dir / "bin" / "powerprofilesctl",
              "#!/bin/sh\ncase \"$1\" in\n  get) cat '" + profile +
                  "' ;;\n"
                  "  set) [ -e '" +
                  (state / "no-performance").string() +
                  "' ] && [ \"$2\" = performance ] && exit 1\n"
                  "       echo \"$2\" > '" +
                  profile + "' ;;\nesac\n",
              /*executable=*/true);
  test::Touch(state / "steam", "online\n");
  test::Touch(state / "profile", "power-saver\n");
  const test::PathPrepend path(dir / "bin");
  // Steam counts as running by its pid file, which any live pid stands in for.
  const test::EnvVar home("HOME", dir.c_str());
  test::Touch(dir / ".steam" / "steam.pid", std::to_string(::getpid()));

  test::LiveServer server(dir / "server");
  // The game notes what it finds while it runs.
  test::Touch(dir / "game" / "run.sh",
              "#!/bin/sh\ncd '" + state.string() +
                  "' && cat steam profile ran-before 2>/dev/null > seen && mv seen seen-by-game\n",
              /*executable=*/true);
  model::Game game;
  game.id = "celeste";
  game.name = "Celeste";
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.install_path = (dir / "game").string();
  game.exe_path = "run.sh";
  REQUIRE(server.games().Upsert(game).has_value());
  auto client = server.Client();
  const auto set_config = [&](const json& edits) {
    return client.Patch("/v1/games/celeste/config", edits.dump(), "application/json");
  };
  // A script given by its path, not executable, as a .sh file saved from a text editor is.
  test::Touch(dir / "before.sh", "echo pre > '" + (state / "ran-before").string() + "'\n");
  REQUIRE(set_config({{"launch.pre_steps", json::array({"steam-invisible", "performance-profile"})},
                      {"launch.post_steps", json::array({"steam-online", "restore-power-profile"})},
                      {"launch.pre_script", "~/before.sh"},
                      {"launch.post_script", "touch '" + (state / "ran-after").string() + "'"}})
              ->status == 200);
  const auto launch = [&] {
    const auto launched = client.Post("/v1/games/celeste/launch");
    REQUIRE(launched != nullptr);
    INFO(launched->body);
    REQUIRE(launched->status == 200);
    REQUIRE(test::WaitUntil([&] { return fs::exists(state / "seen-by-game"); }));
  };
  const auto back = [&](const std::string& steam) {
    const bool done = test::WaitUntil([&] {
      return fs::exists(state / "ran-after") && Read(state / "steam") == steam &&
             Read(state / "profile") == "power-saver\n";
    });
    if (!done) {
      MESSAGE("ran-after: " << fs::exists(state / "ran-after")
                            << ", steam: " << Read(state / "steam")
                            << "profile: " << Read(state / "profile") << "game log:\n"
                            << Read(proc::GameLogPath(server.games().Dir(), "celeste")));
    }
    return done;
  };

  SUBCASE("while playing they're all on, and afterwards everything is as it was") {
    launch();
    CHECK(Read(state / "seen-by-game") == "invisible\nperformance\npre\n");
    CHECK(back("online\n"));
  }

  SUBCASE("a step that can't be applied doesn't keep the game or the other steps from running") {
    test::Touch(state / "no-performance");
    launch();
    CHECK(Read(state / "seen-by-game") == "invisible\npower-saver\npre\n");
    CHECK(back("online\n"));
  }

  SUBCASE("an after-playing step works on its own") {
    test::Touch(state / "steam", "invisible\n");
    REQUIRE(set_config({{"launch.pre_steps", json::array()},
                        {"launch.post_steps", json::array({"steam-online"})}})
                ->status == 200);
    launch();
    CHECK(Read(state / "seen-by-game") == "invisible\npower-saver\npre\n");
    CHECK(back("online\n"));
  }

  SUBCASE("a failing pre-launch script cancels the launch and changes nothing") {
    REQUIRE(set_config({{"launch.pre_script", "exit 3"}})->status == 200);
    const auto launched = client.Post("/v1/games/celeste/launch");
    REQUIRE(launched != nullptr);
    CHECK(launched->status == 409);
    CHECK(json::parse(launched->body)["error"]["code"] == "pre_launch_failed");
    CHECK_FALSE(fs::exists(state / "seen-by-game"));
    CHECK(Read(state / "steam") == "online\n");
    CHECK(Read(state / "profile") == "power-saver\n");
  }

  SUBCASE("only known steps, each once, can be chosen") {
    CHECK(set_config({{"launch.pre_steps", json::array({"reboot"})}})->status == 400);
    CHECK(set_config({{"launch.post_steps", json::array({"steam-online", "steam-online"})}})
              ->status == 400);
  }
}
