#include "core/Strings.h"

#include <doctest.h>

#include <cstdio>

using namespace mira::strings;

TEST_CASE("TitleCase capitalises lowercase words as Steam names tags, and keeps any other casing") {
  CHECK(TitleCase("open world") == "Open World");
  CHECK(TitleCase("free to play") == "Free to Play");
  CHECK(TitleCase("the witcher") == "The Witcher");
  CHECK(TitleCase("RPG") == "RPG");
  CHECK(TitleCase("eSports") == "eSports");
  CHECK(TitleCase("Co-op  sci-fi") == "Co-op  Sci-fi");
  CHECK(TitleCase("2d 'em") == "2d 'em");
}

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
  CHECK(CleanGameName("Setup") == "Setup");        // nothing else to name it by
  CHECK(CleanGameName("inFAMOUS") == "inFAMOUS");  // mixed case is deliberate, kept
}

TEST_CASE("Similarity rates containment and exact matches highly") {
  CHECK(Similarity("Celeste", "Celeste") == doctest::Approx(1.0));
  CHECK(Similarity("Celeste", "CelesteLauncher") > 0.5);
  CHECK(Similarity("Celeste", "TotallyUnrelated") < 0.3);
}

TEST_CASE("SplitArgs keeps quoted arguments together and JoinArgs reads back as the same list") {
  using Args = std::vector<std::string>;
  CHECK(mira::strings::SplitArgs("-windowed  -w 1280") == Args{"-windowed", "-w", "1280"});
  CHECK(mira::strings::SplitArgs(R"(--save "C:\My Games\save 1" 'a b' "")") ==
        Args{"--save", R"(C:\My Games\save 1)", "a b", ""});
  CHECK(mira::strings::SplitArgs("").empty());

  const Args tricky = {"plain", "has space", R"(a"quote)", "", R"(back\slash)"};
  CHECK(mira::strings::SplitArgs(mira::strings::JoinArgs(tricky)) == tricky);
}

TEST_CASE("ShellQuote and DesktopExecArg keep a path one literal argument") {
  // sh gets the path back unexpanded.
  const std::string path = R"(/home/me/Mira x$(touch pwned)`id`'s "1".AppImage)";
  FILE* out = popen(("printf %s " + ShellQuote(path)).c_str(), "r");
  REQUIRE(out != nullptr);
  char buffer[256] = {};
  const std::size_t read = std::fread(buffer, 1, sizeof buffer - 1, out);
  pclose(out);
  CHECK(std::string(buffer, read) == path);

  CHECK(DesktopExecArg("/opt/Mira.AppImage") == R"("/opt/Mira.AppImage")");
  CHECK(DesktopExecArg(R"(/a b/$x"y`z\w 100%)") == R"("/a b/\\$x\\"y\\`z\\\\w 100%%")");
  CHECK(DesktopExecArg("a\nb") == R"("a\nb")");
}

TEST_CASE("ParseGithubUrl reads a repository or one release, and nothing else") {
  using Parsed = std::optional<std::pair<std::string, std::string>>;
  CHECK(mira::strings::ParseGithubUrl("https://github.com/le-birnes/AdobeWine") ==
        Parsed({"le-birnes/AdobeWine", ""}));
  CHECK(mira::strings::ParseGithubUrl("https://github.com/me/AdobeWine.git") ==
        Parsed({"me/AdobeWine", ""}));
  CHECK(mira::strings::ParseGithubUrl("https://github.com/me/AdobeWine/releases/tag/v0.2.0") ==
        Parsed({"me/AdobeWine", "v0.2.0"}));
  CHECK_FALSE(mira::strings::ParseGithubUrl("https://gitlab.com/me/AdobeWine"));
  CHECK_FALSE(mira::strings::ParseGithubUrl("https://github.com/me"));
}

TEST_CASE("EscapeUrl encodes what curl refuses and leaves a valid URL alone") {
  using mira::strings::EscapeUrl;
  CHECK(EscapeUrl("https://cdn.example/EGS_Game (1)_1200x1600") ==
        "https://cdn.example/EGS_Game%20(1)_1200x1600");
  CHECK(EscapeUrl("https://example.com/a%20b?x=1&y=2#top") ==
        "https://example.com/a%20b?x=1&y=2#top");
  CHECK(EscapeUrl("https://example.com/caf\xc3\xa9") == "https://example.com/caf%C3%A9");
}

TEST_CASE("CompareVersions compares the numbers in versions by value") {
  using mira::strings::CompareVersions;
  CHECK(CompareVersions("0.16.10", "0.16.9") > 0);
  CHECK(CompareVersions("1.2", "1.2") == 0);
  CHECK(CompareVersions("v1.2", "v1.10") < 0);
  CHECK(CompareVersions("1.2", "1.2.1") < 0);
}
