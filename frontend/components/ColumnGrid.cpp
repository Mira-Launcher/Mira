#include "ColumnGrid.h"

#include <QWidget>
#include <algorithm>
#include <limits>

#include "Metrics.h"

namespace mira_gui {

std::vector<int> FlowColumns(const std::vector<int>& heights, int columns, int gap) {
  const int n = static_cast<int>(heights.size());
  std::vector<int> out(heights.size(), 0);
  const int k = std::min(std::max(columns, 1), n);
  if (k <= 1) return out;

  std::vector<long> prefix(n + 1, 0);
  for (int i = 0; i < n; ++i) prefix[i + 1] = prefix[i] + heights[i];
  const auto run = [&](int from, int to) {  // cards [from, to) stacked
    return prefix[to] - prefix[from] + static_cast<long>(gap) * (to - from - 1);
  };

  // best[c][j]: the tallest run when the first j cards fill c columns; cut[c][j] where the last
  // of those runs starts.
  constexpr long kNone = std::numeric_limits<long>::max();
  std::vector<std::vector<long>> best(k + 1, std::vector<long>(n + 1, kNone));
  std::vector<std::vector<int>> cut(k + 1, std::vector<int>(n + 1, 0));
  for (int j = 1; j <= n; ++j) best[1][j] = run(0, j);
  for (int c = 2; c <= k; ++c) {
    for (int j = c; j <= n; ++j) {
      for (int i = c - 1; i < j; ++i) {
        if (best[c - 1][i] == kNone) continue;
        const long tallest = std::max(best[c - 1][i], run(i, j));
        if (tallest <= best[c][j]) {  // a later cut on a tie, so earlier columns take more
          best[c][j] = tallest;
          cut[c][j] = i;
        }
      }
    }
  }
  for (int c = k, j = n; c >= 1; --c) {
    const int from = c == 1 ? 0 : cut[c][j];
    for (int i = from; i < j; ++i) out[i] = c - 1;
    j = from;
  }
  return out;
}

ColumnGrid::ColumnGrid(QWidget* parent, int max_columns)
    : QLayout(parent), max_columns_(std::max(1, max_columns)) {
  setContentsMargins(0, 0, 0, 0);
}

ColumnGrid::~ColumnGrid() {
  while (QLayoutItem* item = takeAt(0)) delete item;
}

void ColumnGrid::addItem(QLayoutItem* item) {
  entries_.append({item, false, {}});
  flow_ = {};
  invalidate();
}

void ColumnGrid::InsertHeader(int index, QWidget* widget) {
  addChildWidget(widget);
  int headers = 0;
  int at = 0;
  for (; at < entries_.size(); ++at) {
    if (!entries_[at].header) continue;
    if (headers == index) break;
    ++headers;
  }
  // Past the last header: right after it, ahead of the cards.
  if (index < 0 || at == entries_.size()) {
    at = 0;
    for (int i = 0; i < entries_.size(); ++i)
      if (entries_[i].header) at = i + 1;
  }
  entries_.insert(at, {new QWidgetItem(widget), true, {}});
  flow_ = {};
  invalidate();
}

void ColumnGrid::Place(QWidget* widget, int columns, int column) {
  for (Entry& entry : entries_) {
    if (entry.item->widget() == widget) {
      entry.placement.insert(columns, column);
      invalidate();
      return;
    }
  }
}

void ColumnGrid::Reflow() {
  flow_ = {};
  invalidate();
}

void ColumnGrid::SetMaxColumns(int max) {
  max_columns_ = std::max(1, max);
  invalidate();
}

void ColumnGrid::SetColumnWidth(int width) {
  column_width_ = width;
  invalidate();
}

int ColumnGrid::ColumnWidth() const {
  return column_width_ > 0 ? column_width_ : CardColumnWidth();
}

int ColumnGrid::count() const {
  return static_cast<int>(entries_.size());
}

QLayoutItem* ColumnGrid::itemAt(int index) const {
  return index >= 0 && index < entries_.size() ? entries_[index].item : nullptr;
}

QLayoutItem* ColumnGrid::takeAt(int index) {
  if (index < 0 || index >= entries_.size()) return nullptr;
  flow_ = {};
  return entries_.takeAt(index).item;
}

namespace {

int HeightOf(const QLayoutItem* item, int width) {
  return item->hasHeightForWidth() ? item->heightForWidth(width) : item->sizeHint().height();
}

}  // namespace

ColumnGrid::Layout ColumnGrid::Fit(int width) const {
  Layout fit;
  fit.column_of.assign(entries_.size(), -1);
  std::vector<int> cards;  // entries showing as cards, in order
  for (int i = 0; i < entries_.size(); ++i)
    if (!entries_[i].header && !entries_[i].item->isEmpty()) cards.push_back(i);

  const int column = ColumnWidth();
  const int by_width =
      width <= 0 ? max_columns_
                 : std::clamp((width + kColumnGap) / (column + kColumnGap), 1, max_columns_);
  const int columns = std::max(1, std::min(by_width, static_cast<int>(cards.size())));
  fit.column_width = by_width == 1 && width > 0 ? std::min(column, width) : column;
  fit.frame_columns = by_width;

  const bool placed =
      std::ranges::any_of(cards, [&](int i) { return entries_[i].placement.contains(by_width); });
  if (by_width > 1 && placed) {
    // The page's own placement, with what it left out in the shortest column, then the empty
    // columns dropped.
    std::vector<int> heights(by_width, 0);
    std::vector<int> column_of(entries_.size(), -1);
    for (const int i : cards) {
      const int height = HeightOf(entries_[i].item, fit.column_width) + kCardGap;
      int at = entries_[i].placement.value(by_width, -1);
      if (at < 0 || at >= by_width)
        at = static_cast<int>(std::ranges::min_element(heights) - heights.begin());
      column_of[i] = at;
      heights[at] += height;
    }
    std::vector<int> renumber(by_width, -1);
    int used = 0;
    for (int c = 0; c < by_width; ++c)
      if (heights[c] > 0) renumber[c] = used++;
    for (const int i : cards) fit.column_of[i] = renumber[column_of[i]];
    fit.columns = std::max(1, used);
    return fit;
  }

  fit.columns = columns;
  // The split holds while the same cards show in as many columns, so a card unfolding grows in
  // place instead of moving the cards around it.
  if (flow_.columns != columns || flow_.cards != cards) {
    std::vector<int> heights;
    for (const int i : cards) heights.push_back(HeightOf(entries_[i].item, fit.column_width));
    flow_ = {columns, cards, FlowColumns(heights, columns, kCardGap)};
  }
  for (size_t n = 0; n < cards.size(); ++n) fit.column_of[cards[n]] = flow_.column_of[n];
  return fit;
}

int ColumnGrid::Columns(int width) const {
  return Fit(width).columns;
}

int ColumnGrid::GridWidth(int width) const {
  const Layout fit = Fit(width);
  return ColumnsWidth(fit.columns, fit.column_width);
}

int ColumnGrid::GridLeft(int width) const {
  const Layout fit = Fit(width);
  return std::max(0, (width - ColumnsWidth(fit.frame_columns, fit.column_width)) / 2);
}

int ColumnGrid::Arrange(const QRect& rect, bool apply) const {
  const Layout fit = Fit(rect.width());
  const int grid_width = ColumnsWidth(fit.columns, fit.column_width);
  const int frame_width = ColumnsWidth(fit.frame_columns, fit.column_width);
  const int left = rect.x() + std::max(0, (rect.width() - frame_width) / 2);

  int y = rect.y();
  bool any_header = false;
  for (const Entry& entry : entries_) {
    if (!entry.header || entry.item->isEmpty()) continue;
    if (any_header) y += kTitleGap;
    const int height = HeightOf(entry.item, grid_width);
    if (apply) entry.item->setGeometry(QRect(left, y, grid_width, height));
    y += height;
    any_header = true;
  }
  const int headers_bottom = y;
  if (any_header) y += kTitleGap;

  std::vector<int> bottoms(fit.columns, y);
  std::vector<bool> started(fit.columns, false);
  for (int i = 0; i < entries_.size(); ++i) {
    const int column = fit.column_of[i];
    if (column < 0) continue;
    if (started[column]) bottoms[column] += kCardGap;
    const int height = HeightOf(entries_[i].item, fit.column_width);
    if (apply) {
      const int x = left + column * (fit.column_width + kColumnGap);
      entries_[i].item->setGeometry(QRect(x, bottoms[column], fit.column_width, height));
    }
    bottoms[column] += height;
    started[column] = true;
  }
  if (std::ranges::none_of(started, [](bool s) { return s; })) return headers_bottom - rect.y();
  return *std::ranges::max_element(bottoms) - rect.y();
}

int ColumnGrid::heightForWidth(int width) const {
  return Arrange(QRect(0, 0, width, 0), false);
}

QSize ColumnGrid::sizeHint() const {
  const int width = ColumnsWidth(max_columns_, ColumnWidth());
  return {width, heightForWidth(width)};
}

QSize ColumnGrid::minimumSize() const {
  int width = 0;
  for (const Entry& entry : entries_)
    if (!entry.item->isEmpty()) width = std::max(width, entry.item->minimumSize().width());
  return {std::min(width, ColumnWidth()), 0};
}

void ColumnGrid::setGeometry(const QRect& rect) {
  QLayout::setGeometry(rect);
  Arrange(rect, true);
}

}  // namespace mira_gui
