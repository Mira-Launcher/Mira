#pragma once

#include <QHash>
#include <QLayout>
#include <QList>
#include <vector>

namespace mira_gui {

// Splits cards of these heights, kept in order, into `columns` runs read top to bottom and then
// left to right (newspaper columns), so the tallest run is as short as it can be. On a tie the
// earlier columns take more. Returns each card's column; fewer cards than columns use one each.
std::vector<int> FlowColumns(const std::vector<int>& heights, int columns, int gap);

// Cards in fixed-width columns side by side, top-aligned and each at its own height (never
// stretched), under headers (a page's title) that start at the grid's left edge. As many columns
// fit as the width allows, up to a maximum, and never more than there are cards showing. Those
// columns start at the left of a frame as wide as the width allows, centered, so a grid with fewer
// cards lines up with the first columns of a fuller one beside or above it. Cards flow down the
// first column and on into the next (FlowColumns), unless the page placed them (Place) for that
// many columns. The flowed split is kept while the same cards show in as many columns, so a card
// folding or growing stays put until Reflow.
class ColumnGrid : public QLayout {
 public:
  explicit ColumnGrid(QWidget* parent, int max_columns = 3);
  ~ColumnGrid() override;

  // A card, in the order added (addWidget).
  void addItem(QLayoutItem* item) override;
  // A full-width line over the cards, e.g. the page's title; `index` among the headers.
  void InsertHeader(int index, QWidget* widget);
  void AddHeader(QWidget* widget) { InsertHeader(-1, widget); }
  // With `columns` columns, `widget` goes in column `column` (from 0), below the cards placed there
  // before it. A card left out for a count it's placed for goes in the shortest column. A column
  // left with nothing showing is dropped, so the rest close up to the left.
  void Place(QWidget* widget, int columns, int column);
  // Splits the flowed cards again from their current heights, e.g. once loaded values gave them
  // their real ones.
  void Reflow();
  void SetMaxColumns(int max);
  // A column's width; 0 (the default) follows the text size (CardColumnWidth).
  void SetColumnWidth(int width);

  // How many columns cards take in a layout `width` px wide, and the grid's width and left edge.
  int Columns(int width) const;
  int GridWidth(int width) const;
  int GridLeft(int width) const;

  int count() const override;
  QLayoutItem* itemAt(int index) const override;
  QLayoutItem* takeAt(int index) override;
  Qt::Orientations expandingDirections() const override { return {}; }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override;
  QSize sizeHint() const override;
  QSize minimumSize() const override;
  void setGeometry(const QRect& rect) override;

  static constexpr int kCardGap = 12;

 private:
  struct Entry {
    QLayoutItem* item = nullptr;
    bool header = false;
    QHash<int, int> placement;  // column count -> column
  };
  struct Layout {
    int columns = 1;
    int frame_columns = 1;  // as many as the width allows, which the cards start at the left of
    int column_width = 0;
    std::vector<int> column_of;  // per entry, -1 for a header or a hidden card
  };

  int ColumnWidth() const;
  Layout Fit(int width) const;
  int Arrange(const QRect& rect, bool apply) const;

  struct Flow {
    int columns = 0;
    std::vector<int> cards;      // the entries it split
    std::vector<int> column_of;  // per card
  };

  QList<Entry> entries_;
  mutable Flow flow_;
  int max_columns_ = 3;
  int column_width_ = 0;
};

}  // namespace mira_gui
