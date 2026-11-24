#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "library/ArchiveExtractor.h"
#include "library/Detector.h"
#include "library/FoundArchives.h"
#include "support/TestEnv.h"

using namespace mira;
namespace fs = std::filesystem;

TEST_CASE("archive names: supported formats, split volumes and stems") {
  for (const char* name :
       {"Game.zip", "Game.tar.gz", "Game.tar.xz", "Game.tar.bz2", "Game.tgz", "Game.tar",
        "Game.rar", "Game.7z", "GAME.ZIP", "Game.part1.rar", "Game.part01.rar", "Game.7z.001"}) {
    CHECK_MESSAGE(library::LooksLikeArchive(name), name);
  }
  for (const char* name : {"Game.exe", "Game", "Game.part2.rar", "Game.r00", "Game.7z.002"}) {
    CHECK_MESSAGE(!library::LooksLikeArchive(name), name);
  }
  CHECK(library::IsLaterVolume("Game.part2.rar"));
  CHECK(library::IsLaterVolume("Game.r01"));
  CHECK_FALSE(library::IsLaterVolume("Game.part1.rar"));

  CHECK(library::StemWithoutArchiveExtension("My.Game.Deluxe.Edition.zip") ==
        "My.Game.Deluxe.Edition");
  CHECK(library::StemWithoutArchiveExtension("My.Game.tar.gz") == "My.Game");
  CHECK(library::StemWithoutArchiveExtension("Game.part01.rar") == "Game");
  CHECK(library::StemWithoutArchiveExtension("Game.7z.001") == "Game");

  const fs::path dir = test::TempDir("archive-volumes");
  for (const char* name : {"Game.part1.rar", "Game.part2.rar", "Game.part3.rar", "Other.part2.rar"})
    test::Touch(dir / name);
  CHECK(library::VolumesOf(dir / "Game.part1.rar").size() == 3);
  CHECK(library::FirstVolumeOf(dir / "Game.part3.rar") == dir / "Game.part1.rar");
  CHECK_FALSE(library::FirstVolumeOf(dir / "Other.part2.rar").has_value());
}

TEST_CASE("AnyOpenForWriting sees a file still being written") {
  const fs::path file = test::TempDir("archive-writer") / "Game.zip";
  {
    std::ofstream out(file);
    out << "partial" << std::flush;
    CHECK(library::AnyOpenForWriting({file}));
  }
  CHECK_FALSE(library::AnyOpenForWriting({file}));
}

TEST_CASE("ExtractFound only moves a fully extracted archive into place, and never over a folder") {
  const fs::path dir = test::TempDir("archive-extract");
  test::Touch(dir / "src" / "Game.exe", std::string(200000, 'x'));
  const fs::path archive = dir / "Game.tar.gz";
  REQUIRE(
      std::system(("tar -C " + (dir / "src").string() + " -czf " + archive.string() + " Game.exe")
                      .c_str()) == 0);
  test::TestEnv env("archive-extract-state");
  const auto extract = [&](const fs::path& path, bool delete_archive) {
    return library::ExtractFound(
        env.config, {.path = path, .root = dir, .drop = false, .taken_by = {}, .outputs = {}},
        {.password = {},
         .choices = {.name = "Chosen", .exe_path = "Game.exe"},
         .delete_archive = delete_archive},
        {});
  };

  // A truncated copy fails, leaves no folder behind and keeps the archive.
  const fs::path truncated = dir / "Broken.tar.gz";
  fs::copy_file(archive, truncated);
  fs::resize_file(truncated, fs::file_size(archive) / 2);
  CHECK_FALSE(extract(truncated, true).has_value());
  CHECK_FALSE(fs::exists(dir / "Broken"));
  CHECK_FALSE(fs::exists(dir / (std::string(library::kExtractingPrefix) + "Broken.tar.gz")));
  CHECK(fs::exists(truncated));

  // A folder already there keeps its files; the archive goes beside it, set up as chosen.
  test::Touch(dir / "Game" / "Game.exe", "the installed game");
  CHECK(extract(archive, false).value() == dir / "Game (2)");
  CHECK(fs::file_size(dir / "Game" / "Game.exe") == 18);
  CHECK(fs::file_size(dir / "Game (2)" / "Game.exe") == 200000);
  CHECK(fs::exists(archive));
  CHECK(library::TakeConfirmed(dir / "Game (2)")->name == "Chosen");
  REQUIRE(extract(archive, true).has_value());
  CHECK_FALSE(fs::exists(archive));
}

TEST_CASE("An archive's listing gives its files, and what could start it") {
  const fs::path dir = test::TempDir("archive-list");
  test::Touch(dir / "src" / "Hades" / "Hades.exe", "game");
  test::Touch(dir / "src" / "Hades" / "_CommonRedist" / "vc.exe", "redist");
  test::Touch(dir / "src" / "Hades" / "data.pak", "data");
  // "tar" in a name makes no tarball.
  const fs::path archive = dir / "Star.tarot.zip";
  REQUIRE(std::system(("cd " + (dir / "src").string() + " && 7z a -bso0 " + archive.string() +
                       " Hades > /dev/null")
                          .c_str()) == 0);
  const auto listing = library::ListArchive(archive, {});
  REQUIRE(listing.has_value());
  CHECK(listing->password == std::string());
  REQUIRE(listing->entries.size() == 3);
  std::vector<fs::path> files;
  for (const library::ArchiveEntry& entry : listing->entries) files.push_back(entry.path);
  test::TestEnv env("archive-list-state");
  const auto found =
      library::Detector(library::SettingsFromConfig(env.config)).DetectListed(files, "Hades");
  REQUIRE(found.candidates.size() == 1);
  CHECK(found.candidates[0].rel_path == "Hades/Hades.exe");
  CHECK(std::ranges::contains(library::OutputsOf(archive), dir / "Hades"));
}
