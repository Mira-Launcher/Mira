#include <doctest.h>

#include "library/OwnedTitles.h"

using namespace mira_gui;

namespace {

StoreTitle Title(const char* source, const char* ref, const char* title, bool installed = false) {
  StoreTitle store_title;
  store_title.ref = ref;
  store_title.title = title;
  store_title.installed = installed;
  store_title.owned = true;
  store_title.source = source;
  return store_title;
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

TEST_CASE("Every not-installed game lists in the stores' order, rated from whichever copy is") {
  StoreTitle rated = Title("gog", "1242541569", "The Outer Worlds");
  rated.protondb_tier = "gold";
  rated.review_summary = "Very Positive";
  rated.review_percent = 85;
  const std::vector<StoreTitle> titles = {
      Title("epic", "styx", "Styx: Shards of Darkness"),
      Title("epic", "Rosemallow", "The Outer Worlds"),
      rated,
      Title("gog", "1207658924", "Celeste"),
  };
  const std::vector<OwnedMatch> all = GroupOwned(titles, "");
  REQUIRE(all.size() == 3);
  CHECK(all[0].title == "Styx: Shards of Darkness");
  CHECK(all[1].title == "The Outer Worlds");
  CHECK(all[1].protondb_tier == "gold");
  CHECK(all[1].review_summary == "Very Positive");
  CHECK(all[1].review_percent == 85);
  CHECK(all[2].title == "Celeste");
}

TEST_CASE("A game installed from one store isn't offered from another") {
  const std::vector<StoreTitle> titles = {
      Title("gog", "1207658924", "Celeste"),
      Title("epic", "Salt", "Celeste", /*installed=*/true),
  };
  CHECK(MatchOwned(titles, "celeste").empty());
}
