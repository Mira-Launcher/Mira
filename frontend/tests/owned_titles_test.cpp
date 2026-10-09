#include <doctest.h>

#include "library/OwnedTitles.h"

using namespace mira_gui;

namespace {

StoreTitle Title(const char* source, const char* ref, const char* title, bool installed = false) {
  return {.ref = ref, .title = title, .installed = installed, .owned = true, .source = source, .protondb_tier = {}};
}

}  // namespace

TEST_CASE("A search finds not-installed store titles, one entry per game across stores") {
  const std::vector<StoreTitle> titles = {
      Title("epic", "Rosemallow", "The Outer Worlds"),
      Title("gog", "1242541569", "The Outer Worlds"),
      Title("epic", "cb3b", "The Outer Worlds: Spacer's Choice Edition"),
      Title("gog", "1986509485", "The Outer Worlds: Spacer’s Choice Edition"),
      Title("steam", "753640", "Outer Wilds", /*installed=*/true),
      Title("epic", "styx", "Styx: Shards of Darkness"),
  };

  const std::vector<OwnedMatch> matches = MatchOwned(titles, "outer");
  REQUIRE(matches.size() == 2);  // the installed one is in the library already
  CHECK(matches[0].title == "The Outer Worlds");
  REQUIRE(matches[0].copies.size() == 2);
  CHECK(matches[0].copies[0] == std::pair<QString, QString>("epic", "Rosemallow"));
  CHECK(matches[0].copies[1] == std::pair<QString, QString>("gog", "1242541569"));
  CHECK(matches[1].copies.size() == 2);  // the apostrophes differ, the game doesn't

  CHECK(MatchOwned(titles, "  ").empty());
  CHECK(MatchOwned(titles, "DARK").front().title == "Styx: Shards of Darkness");
}

TEST_CASE("A game installed from one store isn't offered from another") {
  const std::vector<StoreTitle> titles = {
      Title("gog", "1207658924", "Celeste"),
      Title("epic", "Salt", "Celeste", /*installed=*/true),
  };
  CHECK(MatchOwned(titles, "celeste").empty());
}
