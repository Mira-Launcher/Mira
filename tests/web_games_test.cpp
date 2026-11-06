#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <sstream>

#include "library/Detector.h"
#include "runner/Exec.h"
#include "runner/RunnerRegistry.h"
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

std::string Read(const fs::path& file) {
  std::ifstream in(file);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

json Record(httplib::Client& client, const std::string& id) {
  return json::parse(client.Get("/v1/games/" + id)->body);
}

// What the detector picks in `folder`: "<platform> <file>", or "" for nothing.
std::string Picked(const fs::path& folder) {
  const auto found = library::Detector({}).Detect(folder);
  if (found.candidates.empty()) return "";
  return std::string(model::ToString(found.candidates.front().kind)) + " " +
         found.candidates.front().rel_path;
}

}  // namespace

TEST_CASE(
    "Web games: a page that runs a game is found, but not a link, a document or a program's") {
  const fs::path dir = test::TempDir("web-detect");
  test::Touch(dir / "Celeste" / "Celeste.html",
              "<html><body><script>start()</script></body></html>");
  test::Touch(dir / "Celeste" / "img" / "cover.png");
  test::Touch(dir / "Hollow Knight" / "www" / "index.html",
              "<!doctype html><canvas id=game></canvas><script src=index.js></script>");
  CHECK(Picked(dir / "Celeste") == "web Celeste.html");
  CHECK(Picked(dir / "Hollow Knight") == "web www/index.html");

  // Pages beside a game that aren't it.
  test::Touch(
      dir / "Balatro" / "Visit our site.html",
      R"(<meta http-equiv="refresh" content="0; url=https://example.com"><script></script>)");
  test::Touch(dir / "Balatro" / "Play.html",
              "<script>window.location = 'https://store.example.com'</script>");
  test::Touch(dir / "Balatro" / "Game.html", R"(<a href="https://example.com">Our games</a>)");
  test::Touch(dir / "Balatro" / "README.html", "<script>highlight()</script>");
  test::Touch(dir / "Balatro" / "docs" / "index.html", "<script>search()</script>");
  test::Touch(dir / "Balatro" / "manual" / "deep" / "index.html", "<script>search()</script>");
  CHECK(Picked(dir / "Balatro") == "");

  // A program beside a game's own page (NW.js keeps it in www/) is what starts it.
  test::Touch(dir / "Satisfactory" / "Game.exe");
  test::Touch(dir / "Satisfactory" / "www" / "index.html", "<script>boot()</script>");
  CHECK(Picked(dir / "Satisfactory") == "windows Game.exe");
}

TEST_CASE("Web games: a browser of its own opens the served game, on one port, and keeps saves") {
  if (!SystemdUserAvailable()) {
    MESSAGE("no systemd user session; skipped");
    return;
  }
  test::LiveServer server(test::TempDir("web-launch"));
  const fs::path dir = server.config().File().parent_path();
  // Stands in for the browser found first: fetches its page as it was opened, and saves into its
  // profile.
  const fs::path browser = dir / "bin" / "helium-browser";
  test::Touch(browser,
              "#!/bin/sh\n"
              "for arg; do case \"$arg\" in --app=*) url=\"${arg#--app=}\";;"
              " --user-data-dir=*) profile=\"${arg#--user-data-dir=}\";; esac; done\n"
              "echo \"$url\" >> \"$0.urls\"\n"
              "curl -s -D \"$0.headers\" -o \"$0.page\" \"$url\"\n"
              "mkdir -p \"$profile/Default/Local Storage/leveldb\"\n"
              "echo saved > \"$profile/Default/Local Storage/leveldb/000003.log\"\n"
              "origin=$(echo \"$url\" | cut -d/ -f3 | tr : _)\n"
              "db=\"$profile/Default/IndexedDB\"\n"
              "mkdir -p \"$db/http_$origin.indexeddb.leveldb\" "
              "\"$db/chrome-extension_blocker_0.indexeddb.blob\"\n"
              "echo saved > \"$db/http_$origin.indexeddb.leveldb/000003.log\"\n"
              "echo filters > \"$db/chrome-extension_blocker_0.indexeddb.blob/1\"\n",
              /*executable=*/true);
  const test::PathPrepend path(dir / "bin");
  const fs::path game_dir = dir / "Celeste Classic";
  test::Touch(game_dir / "index.html", "<!doctype html><canvas></canvas><script></script>");

  auto client = server.Client();
  const auto added =
      client.Post("/v1/games/manual",
                  json{{"install_path", game_dir.string()}, {"exe_path", "index.html"}}.dump(),
                  "application/json");
  REQUIRE(added != nullptr);
  REQUIRE(added->status == 200);
  const std::string id = json::parse(added->body)["id"];
  CHECK(json::parse(added->body)["platform"] == "web");
  REQUIRE(test::WaitUntil([&] { return Record(client, id)["status"] == "ready"; }));
  const auto saved_files = [&] {
    int files = 0;
    const json saves = json::parse(client.Get("/v1/games/" + id + "/saves")->body);
    for (const json& location : saves["locations"]) files += location["files"].get<int>();
    return files;
  };
  CHECK(saved_files() == 0);  // nothing saved before it's played

  for (int run = 0; run < 2; ++run) {
    const auto launched = client.Post("/v1/games/" + id + "/launch");
    REQUIRE(launched != nullptr);
    REQUIRE(launched->status == 200);
    CHECK(test::WaitUntil([&] { return !Record(client, id)["running"].get<bool>(); }));
  }
  CHECK(Read(browser.string() + ".page") == "<!doctype html><canvas></canvas><script></script>");
  CHECK(Read(browser.string() + ".headers").find("Cross-Origin-Opener-Policy: same-origin") !=
        std::string::npos);
  // The page's address, which its saves are kept under, didn't change between launches.
  std::istringstream urls(Read(browser.string() + ".urls"));
  std::string first;
  std::string second;
  std::getline(urls, first);
  std::getline(urls, second);
  CHECK(first.starts_with("http://127.0.0.1:"));
  CHECK(first == second);
  // With that port taken, the game doesn't start somewhere its saves aren't.
  {
    const int port = std::stoi(first.substr(std::string_view("http://127.0.0.1:").size()));
    httplib::Server squatter;
    REQUIRE(squatter.bind_to_port("127.0.0.1", port));
    const auto refused = client.Post("/v1/games/" + id + "/launch");
    REQUIRE(refused != nullptr);
    CHECK(refused->status == 409);
    CHECK(json::parse(refused->body)["error"]["code"] == "web_port_busy");
  }

  const json saves = json::parse(client.Get("/v1/games/" + id + "/saves")->body);
  bool storage = false;
  for (const json& location : saves["locations"])
    storage = storage || (location["path"].get<std::string>().ends_with("Local Storage") &&
                          location["files"].get<int>() == 1);
  CHECK(storage);
  // Its Local Storage and IndexedDB, not what an extension keeps in the same profile.
  CHECK(saved_files() == 2);
}

TEST_CASE("Web games: a page picked for a game Mira found nothing to run in makes it a web game") {
  test::LiveServer server(test::TempDir("web-pick"));
  const fs::path dir = server.config().File().parent_path();
  test::Touch(dir / "bin" / "chromium", "#!/bin/sh\n", /*executable=*/true);
  const test::PathPrepend path(dir / "bin");
  test::Touch(dir / "Balatro" / "play" / "start.html", "<script>start()</script>");
  model::Game game;
  game.id = "balatro";
  game.name = "Balatro";
  game.status = model::GameStatus::Broken;
  game.last_error = "No executable found automatically. Set one manually.";
  game.install_path = (dir / "Balatro").string();
  game.data_dir = (dir / "prefixes" / "balatro").string();
  REQUIRE(server.games().Upsert(game).has_value());

  auto client = server.Client();
  REQUIRE(
      client.Patch("/v1/games/balatro", R"({"exe_path": "play/start.html"})", "application/json")
          ->status == 200);
  CHECK(Record(client, "balatro")["platform"] == "web");
  CHECK(test::WaitUntil([&] { return Record(client, "balatro")["status"] == "ready"; }));
  CHECK(Record(client, "balatro")["last_error"] == "");
}

TEST_CASE("Web games: with no browser installed, the game says to install one") {
  test::LiveServer server(test::TempDir("web-no-browser"));
  const fs::path dir = server.config().File().parent_path();
  fs::create_directories(dir / "bin");
  const test::EnvVar path("PATH", (dir / "bin").c_str());
  test::Touch(dir / "Celeste" / "Celeste.html", "<script>start()</script>");
  auto client = server.Client();
  const auto added = client.Post(
      "/v1/games/manual",
      json{{"install_path", (dir / "Celeste").string()}, {"exe_path", "Celeste.html"}}.dump(),
      "application/json");
  REQUIRE(added != nullptr);
  const std::string id = json::parse(added->body)["id"];
  REQUIRE(test::WaitUntil([&] { return Record(client, id)["status"] == "broken"; }));

  const auto launched = client.Post("/v1/games/" + id + "/launch");
  REQUIRE(launched != nullptr);
  CHECK(launched->status == 409);
  // Where Mira can't check or install packages, it still says what's missing.
  CHECK(json::parse(launched->body)["error"]["message"].get<std::string>().find(
            "HTML games needs") != std::string::npos);
}
