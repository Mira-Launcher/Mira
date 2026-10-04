#pragma once

#include <vector>

namespace mira_gui::settings_columns {

constexpr int kMinWidth = 600;  // the widest control (the theme tiles) still fits
constexpr int kMaxWidth = 760;
constexpr int kGap = 18;

// Card indices per column, top to bottom.
using Columns = std::vector<std::vector<int>>;

// How a page's cards sit in `room` px: as many columns as fit, never more than
// cards, then the fewest whose tallest column is within 15% of the best.
struct Plan {
  Columns columns;
  int width = 0;  // of each column
};
Plan Arrange(const std::vector<int>& heights, int room);

// Splits cards into `count` columns as evenly as possible. Any card may go in
// any column, but each column keeps the cards' order and the first card stays
// top left; among near-even splits, the one closest to reading order wins.
Columns Balance(const std::vector<int>& heights, int count);

}  // namespace mira_gui::settings_columns
