#include "SettingsColumns.h"

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace mira_gui::settings_columns {

namespace {

constexpr int kNearEven = 24;          // px of extra spread traded for keeping reading order
constexpr long kMaxCandidates = 200000;  // past this, split in reading order instead

int ColumnHeight(const std::vector<int>& heights, const std::vector<int>& cards) {
  int total = 0;
  for (const int i : cards) total += heights[i];
  return cards.empty() ? 0 : total + kGap * static_cast<int>(cards.size() - 1);
}

int Tallest(const std::vector<int>& heights, const Columns& columns) {
  int tallest = 0;
  for (const auto& column : columns) tallest = std::max(tallest, ColumnHeight(heights, column));
  return tallest;
}

// Down the first column, then the next, cut where the tallest column is shortest.
Columns InOrder(const std::vector<int>& heights, int count) {
  const int k = static_cast<int>(heights.size());
  std::vector<std::vector<int>> best(count + 1, std::vector<int>(k + 1, INT_MAX));
  std::vector<std::vector<int>> cut(count + 1, std::vector<int>(k + 1, 0));
  best[0][0] = 0;
  for (int j = 1; j <= count; ++j) {
    for (int i = 1; i <= k; ++i) {
      for (int p = j - 1; p < i; ++p) {
        if (best[j - 1][p] == INT_MAX) continue;
        std::vector<int> run;
        for (int c = p; c < i; ++c) run.push_back(c);
        const int tallest = std::max(best[j - 1][p], ColumnHeight(heights, run));
        if (tallest < best[j][i]) {
          best[j][i] = tallest;
          cut[j][i] = p;
        }
      }
    }
  }
  Columns columns(count);
  for (int j = count, i = k; j > 0; --j) {
    for (int c = cut[j][i]; c < i; ++c) columns[j - 1].push_back(c);
    i = cut[j][i];
  }
  return columns;
}

}  // namespace

Columns Balance(const std::vector<int>& heights, int count) {
  const int k = static_cast<int>(heights.size());
  count = std::clamp(count, 1, std::max(1, k));
  if (count == 1) {
    Columns one(1);
    for (int i = 0; i < k; ++i) one[0].push_back(i);
    return one;
  }
  long candidates = 1;
  for (int i = 0; i < k && candidates <= kMaxCandidates; ++i) candidates *= count;
  if (candidates > kMaxCandidates) return InOrder(heights, count);

  struct Candidate {
    int spread;
    int moved;
    Columns columns;
  };
  std::vector<Candidate> all;
  std::vector<int> pick(k, 0);
  for (long code = 0; code < candidates; ++code) {
    long rest = code;
    for (int i = 0; i < k; ++i) {
      pick[i] = static_cast<int>(rest % count);
      rest /= count;
    }
    if (pick[0] != 0) continue;
    Columns columns(count);
    for (int i = 0; i < k; ++i) columns[pick[i]].push_back(i);
    if (std::ranges::any_of(columns, [](const auto& c) { return c.empty(); })) continue;
    int tallest = 0;
    int shortest = INT_MAX;
    for (const auto& column : columns) {
      tallest = std::max(tallest, ColumnHeight(heights, column));
      shortest = std::min(shortest, ColumnHeight(heights, column));
    }
    int moved = 0;
    int position = 0;
    for (const auto& column : columns) {
      for (const int card : column) moved += std::abs(card - position++);
    }
    all.push_back({tallest - shortest, moved, std::move(columns)});
  }
  const int least = std::ranges::min_element(all, {}, &Candidate::spread)->spread;
  const Candidate* best = nullptr;
  for (const Candidate& candidate : all) {
    if (candidate.spread > least + kNearEven) continue;
    if (best == nullptr || candidate.moved < best->moved ||
        (candidate.moved == best->moved && candidate.spread < best->spread)) {
      best = &candidate;
    }
  }
  return best->columns;
}

Plan Arrange(const std::vector<int>& heights, int room) {
  const int cards = static_cast<int>(heights.size());
  const int fit = std::clamp((room + kGap) / (kMinWidth + kGap), 1, std::max(1, cards));
  const int goal = Tallest(heights, Balance(heights, fit)) * 115 / 100;
  int count = fit;
  for (int m = 1; m < fit; ++m) {
    if (Tallest(heights, Balance(heights, m)) <= goal) {
      count = m;
      break;
    }
  }
  Plan plan;
  plan.columns = Balance(heights, count);
  plan.width = std::max(0, std::min(kMaxWidth, (room - kGap * (count - 1)) / count));
  return plan;
}

}  // namespace mira_gui::settings_columns
