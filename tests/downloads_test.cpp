#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "downloads/Links.h"
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
