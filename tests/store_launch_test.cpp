#include <doctest.h>
#include <httplib.h>
#include <signal.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <string>

#include "epic/Legendary.h"
#include "gog/Gog.h"
#include "runner/Exec.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr const char* kExchangeCode = "SENTINEL0exchange0code";

std::string ReadText(const fs::path& file) {
  std::ifstream in(file);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// An installed store game whose script records the arguments it was started with.
model::Game ScriptGame(const std::string& source, const std::string& ref, const fs::path& folder) {
  test::Touch(folder / "run.sh", "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$0.args\"\n",
              /*executable=*/true);
  model::Game game;
  game.id = source + "-" + ref;
  game.name = ref;
  game.source = source;
  game.source_ref = ref;
  game.platform = model::Platform::Native;
  game.status = model::GameStatus::Ready;
  game.install_path = folder.string();
  game.exe_path = "run.sh";
  return game;
}

// A signed-in stand-in `legendary` whose `launch` runs `body` (with "$*" logged to <bin>.calls).
void UseFakeLegendary(test::LiveServer& server, const fs::path& dir, const std::string& body) {
  const fs::path bin = dir / "legendary";
  test::Touch(bin,
              "#!/bin/sh\n"
              "echo \"$*\" >> \"$0.calls\"\n"
              "case \"$1\" in\n"
              "  launch)\n" +
                  body +
                  " ;;\n"
                  "  --version) echo 'legendary version \"0.21.1\"' ;;\n"
                  "esac\n",
              /*executable=*/true);
  REQUIRE(server.MutableConfig().Set("epic.legendary_bin", bin.string()).has_value());
  test::Touch(epic::LegendaryConfigDir() / "user.json",
              R"({"displayName": "Tester", "refresh_expires_at": "2999-01-01T00:00:00Z"})");
}

std::ptrdiff_t Exits(test::LiveServer& server, const std::string& id) {
  return std::ranges::count_if(server.events().Since(0), [&](const model::Event& event) {
    return event.type == "game.state" && event.payload.value("id", "") == id &&
           event.payload.value("state", "") == "exited";
  });
}

void Launch(httplib::Client& client, const std::string& id) {
  const auto launched = client.Post("/v1/games/" + id + "/launch");
  REQUIRE(launched);
  INFO(launched->body);
  REQUIRE(launched->status == 200);
}

// Launches `id` and waits for it to exit.
void Play(test::LiveServer& server, httplib::Client& client, const std::string& id) {
  const auto before = Exits(server, id);
  Launch(client, id);
  REQUIRE(test::WaitUntil([&] { return Exits(server, id) > before; }, std::chrono::seconds(20)));
}

bool SystemdUserAvailable() {
  Command probe;
  probe.argv = {"systemd-run", "--user", "--scope", "--quiet", "--collect", "--", "true"};
  const auto ran = runner::RunAndWait(probe);
  return ran && ran->exit_code == 0;
}

bool Alive(const fs::path& pid_file) {
  const std::string pid = ReadText(pid_file);
  return !pid.empty() && ::kill(std::stoi(pid), 0) == 0;
}

}  // namespace

TEST_CASE("An Epic game starts signed in to Epic, and its exchange code is never shown") {
  test::LiveServer server(test::TempDir("store-launch-epic"));
  const fs::path dir = server.config().File().parent_path();
  const fs::path ovt = dir / "legendary-tmp" / "ns.ovt";
  const json params = {
      {"game_parameters", json::array()},
      {"egl_parameters",
       {"-AUTH_LOGIN=unused", std::string("-AUTH_PASSWORD=") + kExchangeCode,
        "-AUTH_TYPE=exchangecode", "-epicapp=Fir", "-epicenv=Prod", "-epicovt=" + ovt.string(),
        "-EpicPortal", "-epicusername=Tester", "-epicuserid=abc"}}};
  UseFakeLegendary(server, dir,
                   "    mkdir -p \"" + ovt.parent_path().string() + "\"; touch \"" + ovt.string() +
                       "\"\n    echo '[cli] INFO: Logging in...' >&2\n    echo '" + params.dump() +
                       "'");
  REQUIRE(server.games().Upsert(ScriptGame("epic", "Fir", dir / "fir")).has_value());
  auto client = server.Client();

  Play(server, client, "epic-Fir");
  const std::string args = ReadText(dir / "fir" / "run.sh.args");
  CHECK(args.find(std::string("-AUTH_PASSWORD=") + kExchangeCode) != std::string::npos);
  CHECK(args.find("-epicuserid=abc") != std::string::npos);
  CHECK(ReadText(dir / "legendary.calls").find("launch Fir --json --skip-version-check") !=
        std::string::npos);
  // legendary's ownership token is only for that launch.
  CHECK(test::WaitUntil([&] { return !fs::exists(ovt); }));

  // The code stays out of every log and event.
  const std::string game_log = client.Get("/v1/logs/game:epic-Fir?lines=1000")->body;
  CHECK(game_log.find("-AUTH_PASSWORD=***") != std::string::npos);
  CHECK(game_log.find(kExchangeCode) == std::string::npos);
  CHECK(client.Get("/v1/logs/daemon?lines=5000")->body.find(kExchangeCode) == std::string::npos);
  for (const model::Event& event : server.events().Since(0))
    CHECK(event.payload.dump().find(kExchangeCode) == std::string::npos);

  // A launch refused after legendary wrote the token doesn't leave it behind either.
  const json option = {{"id", "gone"}, {"name", "Gone"}, {"mode", "play"}, {"exe", "missing.sh"}};
  REQUIRE(client
              .Patch("/v1/games/epic-Fir",
                     json{{"launch", {{"options", json::array({option})}}}}.dump(),
                     "application/json")
              ->status == 200);
  const auto refused =
      client.Post("/v1/games/epic-Fir/launch", R"({"option": "gone"})", "application/json");
  REQUIRE(refused);
  CHECK(refused->status == 409);
  CHECK(json::parse(refused->body)["error"]["code"] == "program_missing");
  CHECK_FALSE(fs::exists(ovt));
}

TEST_CASE("A play option gets Epic's sign-in when it's part of the game, unless set otherwise") {
  test::LiveServer server(test::TempDir("store-launch-option"));
  const fs::path dir = server.config().File().parent_path();
  const json params = {
      {"game_parameters", json::array()},
      {"egl_parameters", {std::string("-AUTH_PASSWORD=") + kExchangeCode, "-epicapp=Fir"}}};
  UseFakeLegendary(server, dir, "    echo '" + params.dump() + "'");
  REQUIRE(server.games().Upsert(ScriptGame("epic", "Fir", dir / "fir")).has_value());
  const std::string recorder = "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$0.args\"\n";
  test::Touch(dir / "fir" / "bin" / "dx12.sh", recorder, /*executable=*/true);
  test::Touch(dir / "manager" / "manager.sh", recorder, /*executable=*/true);
  const auto option = [](const std::string& id, const std::string& exe) {
    return json{{"id", id}, {"name", id}, {"mode", "play"}, {"exe", exe}, {"native", true}};
  };
  const std::string manager = (dir / "manager" / "manager.sh").string();
  json options = {option("dx12", "bin/dx12.sh"), option("manager", manager),
                  option("signed", manager), option("unsigned", "{game_dir}/bin/dx12.sh")};
  options[2]["sign_in"] = true;
  options[3]["sign_in"] = false;
  auto client = server.Client();
  REQUIRE(client
              .Patch("/v1/games/epic-Fir", json{{"launch", {{"options", options}}}}.dump(),
                     "application/json")
              ->status == 200);

  // Starts option `id` and returns the arguments its program got.
  const auto play = [&](const std::string& id, const fs::path& program) {
    fs::remove(program.string() + ".args");
    const auto before = Exits(server, "epic-Fir");
    const auto launched =
        client.Post("/v1/games/epic-Fir/launch", json{{"option", id}}.dump(), "application/json");
    REQUIRE(launched);
    INFO(launched->body);
    REQUIRE(launched->status == 200);
    REQUIRE(test::WaitUntil([&] { return Exits(server, "epic-Fir") > before; },
                            std::chrono::seconds(20)));
    return ReadText(program.string() + ".args");
  };
  const std::string signed_in = std::string("-AUTH_PASSWORD=") + kExchangeCode;
  CHECK(play("dx12", dir / "fir" / "bin" / "dx12.sh").find(signed_in) != std::string::npos);
  CHECK(play("manager", manager).find(signed_in) == std::string::npos);
  CHECK(play("signed", manager).find(signed_in) != std::string::npos);
  CHECK(play("unsigned", dir / "fir" / "bin" / "dx12.sh").find(signed_in) == std::string::npos);
  // Only the two signed-in launches asked legendary for a code.
  const std::string calls = ReadText(dir / "legendary.calls");
  size_t asked = 0;
  for (size_t at = calls.find("launch Fir"); at != std::string::npos;
       at = calls.find("launch Fir", at + 1))
    ++asked;
  CHECK(asked == 2);
}

TEST_CASE("An Epic game still starts when legendary can't give its sign-in") {
  test::LiveServer server(test::TempDir("store-launch-epic-offline"));
  const fs::path dir = server.config().File().parent_path();
  // Online fails; offline gives the ids without a code.
  const json offline = {
      {"egl_parameters",
       {"-AUTH_LOGIN=unused", "-AUTH_PASSWORD=", "-epicapp=Fir", "-epicuserid=abc"}}};
  UseFakeLegendary(
      server, dir,
      "    case \"$*\" in *--offline*) echo '" + offline.dump() + "' ;; *) exit 1 ;; esac");
  REQUIRE(server.games().Upsert(ScriptGame("epic", "Fir", dir / "fir")).has_value());
  auto client = server.Client();

  Play(server, client, "epic-Fir");
  CHECK(ReadText(dir / "fir" / "run.sh.args").find("-epicuserid=abc") != std::string::npos);

  // Hanging both ways: the game starts without them once legendary's time is up.
  UseFakeLegendary(server, dir, "    sleep 60");
  fs::remove(dir / "fir" / "run.sh.args");
  const auto started = std::chrono::steady_clock::now();
  Play(server, client, "epic-Fir");
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(30));
  CHECK(ReadText(dir / "fir" / "run.sh.args").find("-epic") == std::string::npos);
}

TEST_CASE("comet runs once beside GOG games, signed in without a token on its command line") {
  if (!SystemdUserAvailable()) {
    MESSAGE("no systemd user session; skipped");
    return;
  }
  test::LiveServer server(test::TempDir("store-launch-gog"));
  const fs::path dir = server.config().File().parent_path();
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const std::string auth = json{
      {"46899977096215655",
       {{"access_token", "SENTINEL0access0token"},
        {"refresh_token", "SENTINEL0refresh0token"},
        {"user_id", "4815"},
        {"loginTime", now},
        {"expires_in", 3600}}}}.dump();
  test::Touch(gog::AuthConfigPath(server.config()), auth);
  test::Touch(dir / "gogdl", "#!/bin/sh\n", /*executable=*/true);
  test::Touch(dir / "endpoints" / "GOG_EMBED" / "userData.json", R"({"username": "Tester"})");
  const fs::path comet = dir / "comet";
  test::Touch(comet,
              "#!/bin/sh\n"
              "[ \"$1\" = --version ] && echo 'comet 0.3.2' && exit 0\n"
              "echo \"$*\" >> \"$0.calls\"\n"
              "cat \"$XDG_CONFIG_PATH/heroic/gog_store/auth.json\" > \"$0.auth\"\n"
              "echo $$ > \"$0.pid\"\n"
              // Notes whether the last game had been told to stop when comet was.
              "trap '[ -e \"" +
                  (dir / "2" / "run.sh.stop").string() +
                  "\" ] && echo after > \"$0.ended\" || echo before > \"$0.ended\"; exit 0' "
                  "TERM\n"
                  "sleep 300 &\n"
                  "wait $!\n",
              /*executable=*/true);
  REQUIRE(server.MutableConfig().Set("gog.gogdl_bin", (dir / "gogdl").string()).has_value());
  REQUIRE(server.MutableConfig().Set("gog.comet_bin", comet.string()).has_value());
  REQUIRE(server.MutableConfig().Set("achievements.enabled", true).has_value());
  test::Touch(
      dir / "endpoints" / "GOG_GAMEPLAY" / "clients" / "1" / "users" / "4815" / "achievements",
      R"({"items": [{"achievement_key": "door", "name": "Door", "visible": true}]})");
  // Each game runs until its stop file appears.
  for (const std::string ref : {"1", "2", "3"}) {
    model::Game game = ScriptGame("gog", ref, dir / ref);
    test::Touch(dir / ref / "run.sh",
                "#!/bin/sh\nwhile [ ! -e \"$0.stop\" ]; do sleep 0.05; done\n",
                /*executable=*/true);
    if (ref == "3") game.overrides["gog.comet"] = false;
    REQUIRE(server.games().Upsert(game).has_value());
  }
  auto client = server.Client();
  // How many times comet was started.
  const auto starts = [&] { return std::ranges::count(ReadText(dir / "comet.calls"), '\n'); };
  const auto stop = [&](const std::string& ref) {
    const auto before = Exits(server, "gog-" + ref);
    test::Touch(dir / ref / "run.sh.stop");
    REQUIRE(test::WaitUntil([&] { return Exits(server, "gog-" + ref) > before; },
                            std::chrono::seconds(20)));
  };

  Launch(client, "gog-1");
  REQUIRE(test::WaitUntil([&] { return Alive(dir / "comet.pid"); }));
  CHECK(starts() == 1);
  const std::string args = ReadText(dir / "comet.calls");
  CHECK(args.find("--username Tester") != std::string::npos);
  CHECK(args.find("SENTINEL0") == std::string::npos);
  CHECK(ReadText(dir / "comet.auth") == auth);

  // A second GOG game shares it, and it outlives the first. The exit hook stops comet (when it
  // does) before refreshing achievements, so their event says it has decided.
  Launch(client, "gog-2");
  stop("1");
  REQUIRE(test::WaitUntil([&] {
    return std::ranges::any_of(server.events().Since(0), [](const model::Event& event) {
      return event.type == "game.achievements_ready" && event.payload.value("id", "") == "gog-1";
    });
  }));
  stop("2");
  CHECK(test::WaitUntil([&] { return !Alive(dir / "comet.pid"); }));
  CHECK(ReadText(dir / "comet.ended") == "after\n");
  CHECK(starts() == 1);

  // Turned off for a game, it isn't started.
  test::Touch(dir / "3" / "run.sh.stop");
  Play(server, client, "gog-3");
  CHECK(starts() == 1);
  for (const model::Event& event : server.events().Since(0))
    CHECK(event.payload.dump().find("SENTINEL0") == std::string::npos);
}
