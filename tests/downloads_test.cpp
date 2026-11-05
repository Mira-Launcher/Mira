#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "downloads/Links.h"
#include "downloads/Queue.h"
#include "downloads/Service.h"
#include "library/ArchiveExtractor.h"
#include "runner/Exec.h"
#include "support/TestEnv.h"

using namespace mira;
using namespace mira::downloads;
using test::TempDir;
namespace fs = std::filesystem;

TEST_CASE("Links are told apart, and a host's address and a file's name are read") {
  CHECK(Classify("magnet:?xt=urn:btih:abc") == LinkKind::Torrent);
  CHECK(Classify("https://example.com/game.TORRENT") == LinkKind::Torrent);
  CHECK(IsWebLink("HTTPS://example.com/a"));
  CHECK(IsWebLink("magnet:?xt=urn:btih:abc"));
  CHECK_FALSE(IsWebLink("file:///etc/passwd"));
  CHECK(NameFromDisposition("attachment; filename*=UTF-8''My%20Game.zip") == "My Game.zip");
  CHECK(Classify("https://pixeldrain.com/u/abc123") == LinkKind::Host);
  CHECK(Classify("https://www.mediafire.com/file/abc/game.7z/file") == LinkKind::Host);
  CHECK(Classify("https://example.com/game.7z") == LinkKind::Direct);

  CHECK(MediaFireAddressIn(R"(<a href="https://download123.mediafire.com/x/game.7z">)") ==
        "https://download123.mediafire.com/x/game.7z");
  CHECK(MediaFireAddressIn("<a href=\"https://example.com/x\">").empty());
  CHECK(NameFromDisposition(R"(attachment; filename="Game Part 1.rar")") == "Game Part 1.rar");
  CHECK(NameFromDisposition("inline").empty());
}

TEST_CASE("FindPassword picks the password that opens an archive") {
  const auto seven_zip = runner::SevenZip();
  if (!seven_zip) return;
  const fs::path dir = TempDir("find-password");
  const fs::path file = dir / "game.bin";
  std::ofstream(file) << "the game";
  const auto pack = [&](const std::string& name, std::vector<std::string> options) {
    Command command;
    command.argv = {*seven_zip, "a", "-bso0", (dir / name).string(), file.string()};
    command.argv.insert(command.argv.end(), options.begin(), options.end());
    REQUIRE(runner::RunAndWait(command)->exit_code == 0);
    return dir / name;
  };
  const std::vector<std::string> saved = {"wrong", "sekret"};

  CHECK(*library::FindPassword(pack("open.7z", {}), saved) == std::string());
  CHECK(*library::FindPassword(pack("files.7z", {"-psekret"}), saved) == std::string("sekret"));
  const fs::path names = pack("names.7z", {"-psekret", "-mhe=on"});
  CHECK(*library::FindPassword(names, saved) == std::string("sekret"));
  CHECK_FALSE(library::FindPassword(names, {"wrong"})->has_value());
}

TEST_CASE("a file name from a server stays one path part") {
  CHECK(CleanFileName("../../.bashrc", "") == "bashrc");
  CHECK(CleanFileName("C:\\x\\game.7z", "") == "game.7z");
  CHECK(CleanFileName("..", "") == "download");
  CHECK(CleanFileName("", "https://a.example/files/My%20Game.7z?token=1") == "My Game.7z");
  CHECK(CleanFileName("", "https://a.example/") == "download");
}

TEST_CASE("A service's answer is read, and its file paths stay under the download") {
  CHECK(SafeRelativePath("Game/sub/setup.exe") == "Game/sub/setup.exe");
  CHECK(SafeRelativePath("../../etc/./passwd") == "etc/passwd");
  CHECK(SafeRelativePath("/abs\\..\\x.bin") == "abs/x.bin");
  CHECK(SafeRelativePath("..") == "download");

  const auto item = nlohmann::json::parse(R"({
    "download_state": "cached", "download_finished": true, "download_present": true, "progress": 1,
    "files": [{"id": 1, "name": "Game/setup.exe", "size": 5},
              {"id": 0, "name": "Game/data/a.bin", "size": 7}]})");
  const ServiceStatus ready = ParseTorBoxItem(item);
  CHECK(ready.ready);
  CHECK(ready.failure.empty());
  REQUIRE(ready.files.size() == 2);
  CHECK(ready.files[0].id == "1");
  CHECK(ready.files[0].path == "setup.exe");
  CHECK(ready.files[1].path == "data/a.bin");

  const ServiceStatus partial = ParseTorBoxItem(nlohmann::json::parse(
      R"({"download_state": "downloading", "download_finished": false, "progress": 0.25,
          "error": null})"));
  CHECK_FALSE(partial.ready);
  CHECK(partial.progress == 0.25);
  CHECK(partial.failure.empty());
  CHECK_FALSE(ParseTorBoxItem(nlohmann::json{{"download_state", "expired"}}).failure.empty());
}

TEST_CASE("The same file from two hosts is one file with mirrors") {
  const auto file = [](const char* url, const char* name, std::int64_t size) {
    return QueuedFile{.url = url,
                      .name = name,
                      .source = "",
                      .gid = "",
                      .status = {.state = "", .total = size, .done = 0, .speed = 0, .error = ""}};
  };
  const auto merged =
      MergeMirrors({file("https://a/x", "game.7z", 5), file("https://b/x", "game.7z", 5),
                    file("https://c/x", "game.7z", 0), file("https://d/y", "game.7z", 9),
                    file("https://e/z", "other.bin", 1)});
  REQUIRE(merged.size() == 3);
  CHECK(merged[0].mirrors == std::vector<std::string>{"https://b/x", "https://c/x"});
  CHECK(merged[1].name == "game (2).7z");
  CHECK(merged[2].name == "other.bin");
}
