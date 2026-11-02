#include <doctest.h>

#include <filesystem>
#include <json.hpp>

#include "runner/Exec.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

bool SystemdUserAvailable() {
  Command probe;
  probe.argv = {"systemd-run", "--user", "--scope", "--quiet", "--collect", "--", "true"};
  auto ran = runner::RunAndWait(probe);
  return ran.has_value() && ran->exit_code == 0;
}

json Record(httplib::Client& client, const std::string& id) {
  return json::parse(client.Get("/v1/games/" + id)->body);
}

}  // namespace

TEST_CASE(
    "A tool a game opens is listed as open until it closes, and stopping the game closes it") {
  if (!SystemdUserAvailable()) {
    MESSAGE("no systemd user session; skipped");
    return;
  }
  test::LiveServer server(test::TempDir("launch-tools"));
  const fs::path dir = server.config().File().parent_path();
  // A launcher that starts the real program and exits at once, as many mod managers' do.
  test::Touch(dir / "manager" / "launch.sh", "#!/bin/sh\nsleep 300 &\ntouch \"$0.done\"\n",
              /*executable=*/true);
  test::Touch(dir / "game" / "run.sh", "#!/bin/sh\n", /*executable=*/true);
  model::Game game;
  game.id = "fir";
  game.name = "Fir";
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.install_path = (dir / "game").string();
  game.exe_path = "run.sh";
  REQUIRE(server.games().Upsert(game).has_value());
  auto client = server.Client();
  const json option = {{"id", "manager"},
                       {"name", "Manager"},
                       {"mode", "tool"},
                       {"exe", (dir / "manager" / "launch.sh").string()},
                       {"native", true}};
  REQUIRE(client
              .Patch("/v1/games/fir", json{{"launch", {{"options", json::array({option})}}}}.dump(),
                     "application/json")
              ->status == 200);
  CHECK(Record(client, "fir")["tools"].empty());
  // The tools a game.updated since event `after` last announced.
  const auto announced = [&](std::int64_t after) {
    std::optional<json> tools;
    for (const model::Event& event : server.events().Since(after)) {
      if (event.type == "game.updated") tools = event.payload["tools"];
    }
    return tools;
  };
  const auto last_id = [&] {
    const auto events = server.events().Since(0);
    return events.empty() ? std::int64_t{0} : events.back().id;
  };

  const std::int64_t before_open = last_id();
  const auto opened =
      client.Post("/v1/games/fir/launch", R"({"option": "manager"})", "application/json");
  REQUIRE(opened != nullptr);
  CHECK(json::parse(opened->body)["status"] == "opened");
  CHECK(Record(client, "fir")["tools"] == json::array({{{"id", "manager"}, {"name", "Manager"}}}));
  CHECK(announced(before_open) == json::array({{{"id", "manager"}, {"name", "Manager"}}}));

  // Still open once its launcher has exited, since what it started runs on; not a play session.
  REQUIRE(test::WaitUntil([&] { return fs::exists(dir / "manager" / "launch.sh.done"); }));
  const std::int64_t after_launcher = last_id();
  CHECK_FALSE(test::WaitUntil([&] { return Record(client, "fir")["tools"].empty(); },
                              std::chrono::milliseconds(2500)));
  CHECK_FALSE(Record(client, "fir")["running"].get<bool>());
  for (const model::Event& event : server.events().Since(0)) CHECK(event.type != "game.state");

  const auto stopped = client.Post("/v1/games/fir/stop");
  REQUIRE(stopped != nullptr);
  CHECK(json::parse(stopped->body)["status"] == "stopping");
  CHECK(test::WaitUntil([&] { return Record(client, "fir")["tools"].empty(); }));
  CHECK(test::WaitUntil([&] { return announced(after_launcher) == json::array(); }));

  // Nothing left open: stopping again is the usual not_running.
  CHECK(json::parse(client.Post("/v1/games/fir/stop")->body)["status"] == "not_running");
}

TEST_CASE("A tool that can't get a systemd scope is an error, not opened") {
  test::LiveServer server(test::TempDir("launch-tools-no-scope"));
  const fs::path dir = server.config().File().parent_path();
  // systemd-run without a user session fails before it runs anything.
  test::Touch(dir / "bin" / "systemd-run",
              "#!/bin/sh\necho 'Failed to connect to user scope bus' >&2\nexit 1\n",
              /*executable=*/true);
  test::PathPrepend path(dir / "bin");
  test::Touch(dir / "manager" / "launch.sh", "#!/bin/sh\ntouch \"$0.ran\"\n", /*executable=*/true);
  test::Touch(dir / "game" / "run.sh", "#!/bin/sh\n", /*executable=*/true);
  model::Game game;
  game.id = "fir";
  game.name = "Fir";
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.install_path = (dir / "game").string();
  game.exe_path = "run.sh";
  REQUIRE(server.games().Upsert(game).has_value());
  auto client = server.Client();
  const json option = {{"id", "manager"},
                       {"name", "Manager"},
                       {"mode", "tool"},
                       {"exe", (dir / "manager" / "launch.sh").string()},
                       {"native", true}};
  REQUIRE(client
              .Patch("/v1/games/fir", json{{"launch", {{"options", json::array({option})}}}}.dump(),
                     "application/json")
              ->status == 200);

  const auto opened =
      client.Post("/v1/games/fir/launch", R"({"option": "manager"})", "application/json");
  REQUIRE(opened != nullptr);
  CHECK(opened->status == 500);
  CHECK(json::parse(opened->body)["error"]["code"] == "cgroup_unavailable");
  CHECK(Record(client, "fir")["tools"].empty());
  CHECK_FALSE(fs::exists(dir / "manager" / "launch.sh.ran"));
}
