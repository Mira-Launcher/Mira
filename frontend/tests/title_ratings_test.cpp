#include <doctest.h>

#include "library/TitleRatings.h"

using namespace mira_gui;

TEST_CASE("Not-installed filters let a title through when each picked group holds it") {
  TitleFilter filter;
  CHECK(filter.Empty());
  CHECK(filter.Matches("epic", "", ""));

  // Within a group any pick will do; across groups every group has to.
  filter.tiers = {"platinum", "gold"};
  filter.reviews = {ReviewBucket::VeryPositive};
  CHECK(filter.Matches("epic", "gold", "Very Positive"));
  CHECK_FALSE(filter.Matches("epic", "silver", "Very Positive"));
  CHECK_FALSE(filter.Matches("epic", "gold", "Mostly Positive"));

  filter.stores = {"gog"};
  CHECK_FALSE(filter.Matches("epic", "gold", "Very Positive"));
  CHECK(filter.Matches("gog", "gold", "Very Positive"));

  // Every negative label is one group, and a title too little reviewed for a label isn't rated.
  filter = {};
  filter.reviews = {ReviewBucket::Negative};
  CHECK(filter.Matches("epic", "", "Very Negative"));
  CHECK(filter.Matches("epic", "", "Mostly Negative"));
  filter.reviews = {ReviewBucket::NotRated};
  CHECK(filter.Matches("epic", "", "4 user reviews"));
  CHECK(filter.Matches("epic", "", ""));
  CHECK_FALSE(filter.Matches("epic", "", "Positive"));

  // A pending tier, or none, is not rated yet; ProtonDB's old "garbage" is borked.
  filter = {};
  filter.tiers = {""};
  CHECK(filter.Matches("epic", "pending", ""));
  CHECK(filter.Matches("epic", "", ""));
  CHECK_FALSE(filter.Matches("epic", "bronze", ""));
  filter.tiers = {"borked"};
  CHECK(filter.Matches("epic", "garbage", ""));
}
