#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "config/Config.h"
#include "itch/Butlerd.h"
#include "itch/Itch.h"
#include "itch/ItchImporter.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

namespace {

// A stand-in for the real `butler`, pointed at by itch.butler_bin.
// Unlike Legendary/gogdl's one-shot-subprocess fakes, this has to be a
// real (if tiny) JSON-RPC server, not just a script that prints canned
// text -- Butlerd.cpp's whole job is a live daemon connection (spawn,
// read the listen-notification off a mix of log noise, connect a TCP
// socket, Meta.Authenticate, then read a response back off a stream that
// may carry unrelated notifications first), and that's exactly the code
// this pins as a regression the same way epic_test.cpp's fake legendary
// pins ParseJsonTail. Written in Python (itself confirmed present on
// this dev machine via gogdl's own shebang) since a real socket server
// isn't practical in /bin/sh.
void WriteFakeButler(const fs::path& path) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << R"PY(#!/usr/bin/env python3
import json, os, socket, sys, threading

# DetectButler's own VersionOf() runs "<path> --version" directly (not
# through "daemon") to probe the binary -- has to actually answer that and
# exit, or it hangs forever in the socket-server loop below instead.
if len(sys.argv) > 1 and sys.argv[1] == "--version":
    print("butler version 15.31.0")
    sys.exit(0)

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.bind(("127.0.0.1", 0))
srv.listen(1)
port = srv.getsockname()[1]
secret = "test-secret"

print(json.dumps({"type": "log", "message": "butlerd: creating new DB", "time": 0}), flush=True)
print(json.dumps({"type": "butlerd/listen-notification", "secret": secret,
                  "tcp": {"address": "127.0.0.1:%d" % port}}), flush=True)

conn, _ = srv.accept()
buf = b""
f = conn.makefile("rwb")
while True:
    line = f.readline()
    if not line:
        break
    req = json.loads(line)
    method = req.get("method")
    rid = req.get("id")
    if method == "Meta.Authenticate":
        if req["params"].get("secret") != secret:
            f.write((json.dumps({"jsonrpc": "2.0", "id": rid,
                                 "error": {"message": "bad secret"}}) + "\n").encode())
        else:
            f.write((json.dumps({"jsonrpc": "2.0", "id": rid, "result": {}}) + "\n").encode())
    elif method == "Test.WithNotificationFirst":
        # A notification (no "id") pushed before the real response -- the
        # client must skip it, not mistake it for the answer to this call.
        f.write((json.dumps({"jsonrpc": "2.0", "method": "Progress", "params": {}}) + "\n").encode())
        f.write((json.dumps({"jsonrpc": "2.0", "id": rid, "result": {"ok": True}}) + "\n").encode())
    elif method == "Fetch.ProfileOwnedKeys":
        f.write((json.dumps({"jsonrpc": "2.0", "id": rid, "result": {"items": [
            {"game": {"id": 42, "title": "A Fetched Game"}}
        ]}}) + "\n").encode())
    elif method == "Profile.LoginWithAPIKey":
        if req["params"].get("apiKey") == "bad-key":
            f.write((json.dumps({"jsonrpc": "2.0", "id": rid,
                                 "error": {"message": "invalid API key"}}) + "\n").encode())
        else:
            f.write((json.dumps({"jsonrpc": "2.0", "id": rid,
                                 "result": {"profile": {"id": 7}}}) + "\n").encode())
    elif method == "Fetch.Caves":
        # Installed games. mirad keeps one butlerd for the whole process, so
        # whichever test started this one, the list is in the shared test dir.
        shared = os.path.dirname(os.path.dirname(sys.argv[0]))
        with open(os.path.join(shared, "itch-caves.json")) as caves:
            f.write((json.dumps({"jsonrpc": "2.0", "id": rid,
                                 "result": {"items": json.load(caves)}}) + "\n").encode())
    else:
        f.write((json.dumps({"jsonrpc": "2.0", "id": rid, "result": {}}) + "\n").encode())
    f.flush()
)PY";
  out.close();
  fs::permissions(path, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec);
}

struct Fixture : test::TestEnv {
  explicit Fixture(const char* name) : TestEnv(name) {}

  void UseFakeButler() {
    const fs::path bin = dir / "butler";
    WriteFakeButler(bin);
    REQUIRE(config.Set("itch.butler_bin", bin.string()));
  }
};

}  // namespace

TEST_CASE("DetectButler honours the itch.butler_bin override") {
  Fixture fixture("itch-detect");
  fixture.UseFakeButler();

  const itch::ItchStatus status = itch::DetectButler(fixture.config);
  CHECK(status.installed);
  CHECK(status.source == "override");
}

TEST_CASE("Call connects, authenticates the transport, and returns a real result") {
  Fixture fixture("itch-call");
  fixture.UseFakeButler();

  const Result<nlohmann::json> result = itch::Call(fixture.config, "Fetch.ProfileOwnedKeys", {{"fresh", true}});
  REQUIRE(result);
  REQUIRE(result->contains("items"));
  REQUIRE((*result)["items"].size() == 1);
  CHECK((*result)["items"][0]["game"]["title"] == "A Fetched Game");
}

TEST_CASE("Call skips a notification pushed ahead of the real response") {
  Fixture fixture("itch-call-notification");
  fixture.UseFakeButler();

  const Result<nlohmann::json> result = itch::Call(fixture.config, "Test.WithNotificationFirst", {});
  REQUIRE(result);
  CHECK(result->value("ok", false));
}

TEST_CASE("Login stores the key only once butlerd actually accepts it") {
  Fixture fixture("itch-login-bad-key");
  fixture.UseFakeButler();

  const Result<void> result = itch::Login(fixture.config, "bad-key");
  REQUIRE_FALSE(result);
  CHECK_FALSE(fs::exists(itch::ApiKeyFile(fixture.config)));
}

TEST_CASE("Login stores the key once butlerd accepts it, and Status reflects it") {
  Fixture fixture("itch-login-good-key");
  fixture.UseFakeButler();

  REQUIRE(itch::Login(fixture.config, "good-key"));
  CHECK(itch::Status(fixture.config).authenticated);
}

TEST_CASE("ParseCollectionLink reads the id from each link form") {
  CHECK(itch::ParseCollectionLink("https://itch.io/c/8213205/ex03s-collection") == 8213205);
  CHECK(itch::ParseCollectionLink("itch.io/c/8213205") == 8213205);
  CHECK(itch::ParseCollectionLink(" 8213205 ") == 8213205);
  CHECK_FALSE(itch::ParseCollectionLink("https://noelcody.itch.io/moss-moss"));
  CHECK_FALSE(itch::ParseCollectionLink(""));
}

TEST_CASE("An itch.io import tracks installed games, finds what to run, and keeps a fixed exe") {
  Fixture fixture("itch-import");
  fixture.UseFakeButler();
  const fs::path moss = fixture.dir / "installs" / "moss-moss";
  const fs::path ripples = fixture.dir / "installs" / "ripples";
  test::Touch(moss / "MossMoss.x86_64", "", /*executable=*/true);
  test::Touch(ripples / "Ripples.exe");
  const auto cave = [](int id, const char* title, const fs::path& folder, const char* platform) {
    return nlohmann::json{{"id", "cave-" + std::to_string(id)},
                          {"game", {{"id", id}, {"title", title}}},
                          {"installInfo", {{"installFolder", folder.string()}}},
                          {"upload", {{"platforms", {{platform, "all"}}}}}};
  };
  test::Touch(fixture.dir.parent_path() / "itch-caves.json",
              nlohmann::json::array({cave(101, "Moss Moss", moss, "linux"),
                                     cave(202, "Ripples", ripples, "windows")})
                  .dump());

  itch::ItchImporter importer(fixture.config, fixture.games, fixture.events);
  CHECK_FALSE(importer.Import());  // not signed in yet
  REQUIRE(itch::Login(fixture.config, "good-key"));

  const auto first = importer.Import();
  REQUIRE(first);
  CHECK(first->added == 2);
  const auto native = fixture.games.Find("itch-101");
  REQUIRE(native);
  CHECK(native->platform == model::Platform::Native);
  CHECK(native->exe_path == "MossMoss.x86_64");
  CHECK(native->status == model::GameStatus::Ready);
  const auto windows = fixture.games.Find("itch-202");
  REQUIRE(windows);
  CHECK(windows->platform == model::Platform::Windows);
  CHECK(windows->exe_path == "Ripples.exe");
  CHECK_FALSE(windows->data_dir.empty());

  REQUIRE(fixture.games.Update("itch-202",
                               [](model::Game& game) { game.exe_path = "bin/Launcher.exe"; }));
  const auto second = importer.Import();
  REQUIRE(second);
  CHECK(second->updated == 2);
  CHECK(fixture.games.Find("itch-202")->exe_path == "bin/Launcher.exe");
}
