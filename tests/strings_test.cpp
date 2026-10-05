#include <doctest.h>

#include "core/Strings.h"

using namespace mira::strings;

TEST_CASE("GlobMatch handles * and ? like shell globs") {
  CHECK(GlobMatch("*.exe", "Celeste.exe"));
  CHECK(GlobMatch("unins*", "uninstall.exe"));
  CHECK_FALSE(GlobMatch("unins*", "install.exe"));
  CHECK(GlobMatch("*/prefix/*", "Games/prefix/Celeste/drive_c"));
  CHECK(GlobMatch("a?c", "abc"));
  CHECK_FALSE(GlobMatch("a?c", "abbc"));
}

TEST_CASE("Slugify produces filesystem-safe, human-readable ids") {
  CHECK(Slugify("Celeste") == "celeste");
  CHECK(Slugify("Grand Theft Auto: V") == "grand-theft-auto-v");
  CHECK(Slugify("  spaced  out  ") == "spaced-out");
  CHECK(Slugify("!!!") == "game");  // nothing alnum survives; must not be empty
}

TEST_CASE("CleanGameName strips repack/store noise but keeps the title") {
  CHECK(CleanGameName("Celeste-v1.4.0.0-AnkerGames") == "Celeste");
  CHECK(CleanGameName("Hollow_Knight_GOG") == "Hollow Knight");
  CHECK(CleanGameName("Half-Life 2") == "Half Life 2");  // real hyphenated title kept
}

TEST_CASE("CleanGameName turns an installer's folder into the game's name") {
  CHECK(CleanGameName("setup_crate_escape") == "Crate Escape");
  CHECK(CleanGameName("setup_clustertruck_2.1.0_(64bit)_(12345)") == "Clustertruck");
  CHECK(CleanGameName("Install Celeste") == "Celeste");
  CHECK(CleanGameName("Setup") == "Setup");  // nothing else to name it by
  CHECK(CleanGameName("inFAMOUS") == "inFAMOUS");  // mixed case is deliberate, kept
}

TEST_CASE("Similarity rates containment and exact matches highly") {
  CHECK(Similarity("Celeste", "Celeste") == doctest::Approx(1.0));
  CHECK(Similarity("Celeste", "CelesteLauncher") > 0.5);
  CHECK(Similarity("Celeste", "TotallyUnrelated") < 0.3);
}
