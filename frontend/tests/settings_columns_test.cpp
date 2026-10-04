#include <doctest.h>

#include <algorithm>

#include "ui/SettingsColumns.h"

using namespace mira_gui::settings_columns;

namespace {

int Height(const std::vector<int>& heights, const std::vector<int>& column) {
  int total = 0;
  for (const int i : column) total += heights[i];
  return total + kGap * static_cast<int>(column.size() - 1);
}

int Spread(const std::vector<int>& heights, const Columns& columns) {
  std::vector<int> totals;
  for (const auto& column : columns) totals.push_back(Height(heights, column));
  return *std::ranges::max_element(totals) - *std::ranges::min_element(totals);
}

// Every card once, in its original order within each column, the first card top left.
void CheckValid(const Columns& columns, int cards) {
  std::vector<int> seen;
  for (const auto& column : columns) {
    CHECK_FALSE(column.empty());
    CHECK(std::ranges::is_sorted(column));
    seen.insert(seen.end(), column.begin(), column.end());
  }
  std::ranges::sort(seen);
  CHECK(seen.size() == static_cast<size_t>(cards));
  for (int i = 0; i < cards; ++i) CHECK(seen[i] == i);
  CHECK(columns.front().front() == 0);
}

}  // namespace

TEST_CASE("Settings columns place every card once and keep each column in order") {
  const std::vector<int> heights = {300, 120, 520, 90, 260, 140, 400};
  for (int count = 1; count <= 4; ++count) CheckValid(Balance(heights, count), 7);
}

TEST_CASE("Settings columns move a card out of reading order when that evens the columns") {
  // In order, the best two-column split is {0,1} | {2,3}: 718 vs 1018 px.
  const std::vector<int> heights = {600, 100, 900, 100};
  const Columns columns = Balance(heights, 2);
  CheckValid(columns, 4);
  CHECK(Spread(heights, columns) < 300);
}

TEST_CASE("Settings columns keep reading order when it is already even") {
  const std::vector<int> heights = {200, 200, 200, 200};
  CHECK(Balance(heights, 2) == Columns{{0, 1}, {2, 3}});
}

TEST_CASE("Settings pages never get more columns than cards, nor ones narrower than the minimum") {
  CHECK(Arrange({400}, 3000).columns.size() == 1);
  CHECK(Arrange({400}, 3000).width == kMaxWidth);
  CHECK(Arrange({300, 300, 300}, 2 * kMinWidth + kGap - 1).columns.size() == 1);
  const Plan wide = Arrange({300, 300, 300}, 3 * kMinWidth + 2 * kGap);
  CHECK(wide.columns.size() == 3);
  CHECK(wide.width >= kMinWidth);
}

TEST_CASE("Settings pages use fewer columns when that is nearly as short") {
  // Three columns would be 500 tall; two are 500 too (one card alone, two stacked).
  const Plan plan = Arrange({500, 241, 241}, 3000);
  CHECK(plan.columns.size() == 2);
}

TEST_CASE("Settings columns stay fast and valid for long pages") {
  std::vector<int> heights(14, 100);
  CheckValid(Balance(heights, 4), 14);
}
