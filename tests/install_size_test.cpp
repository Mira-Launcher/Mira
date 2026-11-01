#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <sstream>

#include "library/InstallSize.h"
#include "library/StoreProgress.h"
#include "support/LiveServer.h"
#include "support/TestEnv.h"

using namespace mira;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// A store tool that logs its arguments and answers its info call with `info`, log lines first.
void FakeTool(test::LiveServer& server, const char* setting, const fs::path& bin,
              const std::string& info) {
  test::Touch(bin,
              "#!/bin/sh\n"
              "[ \"$1\" = --version ] && { echo '1.0.0'; exit 0; }\n"
              "echo \"$*\" >> \"$0.calls\"\n"
              "echo '[cli] INFO: Getting game info' >&2\n"
              "cat <<'EOF'\n" +
                  info + "\nEOF\n",
              /*executable=*/true);
  REQUIRE(server.MutableConfig().Set(setting, bin.string()).has_value());
}

int Lines(const fs::path& file) {
  std::ifstream in(file);
  int lines = 0;
  for (std::string line; std::getline(in, line);) ++lines;
  return lines;
}

}  // namespace

TEST_CASE(
    "A store title's size is asked of its store tool once, and an install's progress has it") {
  test::LiveServer server(test::TempDir("install-size"));
  const fs::path dir = server.config().File().parent_path();
  FakeTool(
      server, "epic.legendary_bin", dir / "legendary",
      R"({"game": {"app_name": "Fir"}, "manifest": {"disk_size": 3000, "download_size": 2000}})");
  FakeTool(server, "gog.gogdl_bin", dir / "gogdl",
           R"({"size": {"en-US": {"download_size": 10, "disk_size": 20},
                        "*": {"download_size": 400, "disk_size": 900}}})");
  FakeTool(server, "amazon.nile_bin", dir / "nile", R"({"download_size": 55, "disk_size": 77})");
  auto client = server.Client();
  const auto size = [&](const std::string& source, const std::string& ref) {
    return client.Get("/v1/library/size?source=" + source + "&ref=" + ref);
  };

  const auto epic = size("epic", "Fir");
  REQUIRE(epic != nullptr);
  REQUIRE(epic->status == 200);
  CHECK(json::parse(epic->body) == json{{"download_bytes", 2000}, {"install_bytes", 3000}});
  CHECK(json::parse(size("gog", "1207")->body) ==
        json{{"download_bytes", 400}, {"install_bytes", 900}});
  CHECK(json::parse(size("amazon", "amzn1.lost")->body) ==
        json{{"download_bytes", 55}, {"install_bytes", 77}});

  // Asked once a session.
  REQUIRE(size("epic", "Fir")->status == 200);
  CHECK(Lines(dir / "legendary.calls") == 1);

  CHECK(size("itch", "123")->status == 400);
  CHECK(client.Get("/v1/library/size?source=epic")->status == 400);

  // An install whose tool never prints its size still reports the total asked for before.
  {
    library::StoreProgress progress(server.events(), "epic", "Fir");
    progress.Feed("= Progress: 10.00% (1/10), Running for 00:00:01, ETA: 00:00:09\n");
  }
  const auto event = test::WaitForEvent(server.events(), "library.install.progress");
  REQUIRE(event.has_value());
  CHECK(event->payload["total"] == 2000);
}

TEST_CASE("A store tool's info without sizes is size_unknown") {
  test::LiveServer server(test::TempDir("install-size-unknown"));
  const fs::path dir = server.config().File().parent_path();
  FakeTool(server, "epic.legendary_bin", dir / "legendary", R"({"game": {"app_name": "Spruce"}})");
  auto client = server.Client();
  const auto answer = client.Get("/v1/library/size?source=epic&ref=Spruce");
  REQUIRE(answer != nullptr);
  CHECK(answer->status == 404);
  CHECK(json::parse(answer->body)["error"]["code"] == "size_unknown");
}
