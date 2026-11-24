#include <doctest.h>

#include <QWidget>
#include <vector>

#include "NavWindow.h"
#include "components/ColumnGrid.h"
#include "components/Metrics.h"

using namespace mira_gui;

namespace {

constexpr int kCardWidth = 516;

// A grid of cards of these heights in a host `width` px wide, laid out.
struct Grid {
  explicit Grid(const std::vector<int>& heights) {
    grid = new ColumnGrid(&host);
    grid->SetColumnWidth(kCardWidth);
    for (const int height : heights) {
      auto* card = new QWidget(&host);
      card->setFixedHeight(height);
      grid->addWidget(card);
      cards.push_back(card);
    }
  }
  void Lay(int width) { grid->setGeometry(QRect(0, 0, width, grid->heightForWidth(width))); }

  QWidget host;
  ColumnGrid* grid = nullptr;
  std::vector<QWidget*> cards;
};

int Width(int columns) {
  return columns * kCardWidth + (columns - 1) * kColumnGap;
}

}  // namespace

TEST_CASE("Cards flow down one column and on into the next, balanced, in their order") {
  CHECK(FlowColumns({100, 100, 100, 100}, 2, 16) == std::vector<int>{0, 0, 1, 1});
  CHECK(FlowColumns({300, 100, 100, 100}, 2, 16) == std::vector<int>{0, 1, 1, 1});
  CHECK(FlowColumns({100, 100, 100, 100, 100, 100}, 3, 16) == std::vector<int>{0, 0, 1, 1, 2, 2});
  // A tie leaves the earlier column the longer one.
  CHECK(FlowColumns({100, 100, 100}, 2, 16) == std::vector<int>{0, 0, 1});
  CHECK(FlowColumns({100, 100}, 3, 16) == std::vector<int>{0, 1});
  CHECK(FlowColumns({}, 3, 16).empty());
}

TEST_CASE("A column grid fits as many columns as the width allows and never stretches a card") {
  nav_test::App();
  Grid grid({120, 340, 80, 200, 60});

  CHECK(grid.grid->Columns(Width(3) + 200) == 3);
  CHECK(grid.grid->Columns(Width(3) - 1) == 2);
  CHECK(grid.grid->Columns(Width(2) - 1) == 1);

  grid.Lay(Width(3) + 200);
  CHECK(grid.grid->GridLeft(Width(3) + 200) == 100);
  for (QWidget* card : grid.cards) {
    CHECK(card->width() == kCardWidth);
    CHECK(card->height() == card->minimumHeight());
  }
  // Top-aligned, and read top to bottom then left to right.
  int previous_x = -1;
  int previous_bottom = 0;
  for (QWidget* card : grid.cards) {
    if (card->x() == previous_x) CHECK(card->y() > previous_bottom);
    if (card->x() != previous_x) CHECK(card->y() == 0);
    CHECK(card->x() >= previous_x);
    previous_x = card->x();
    previous_bottom = card->geometry().bottom();
  }

  // One column narrower than a card takes the room there is.
  grid.Lay(400);
  CHECK(grid.cards[0]->width() == 400);
  CHECK(grid.cards[1]->x() == 0);
}

TEST_CASE("A column grid uses no more columns than it has cards showing") {
  nav_test::App();
  Grid grid({100, 100, 100});
  CHECK(grid.grid->Columns(Width(3)) == 3);
  grid.cards[2]->hide();
  CHECK(grid.grid->Columns(Width(3)) == 2);
  CHECK(grid.grid->GridWidth(Width(3)) == Width(2));

  // The two start where a full grid's first column does, so pages line up whatever they hold.
  Grid full({100, 100, 100});
  full.Lay(Width(3) + 200);
  grid.Lay(Width(3) + 200);
  CHECK(grid.cards[0]->x() == full.cards[0]->x());
  CHECK(grid.grid->GridLeft(Width(3) + 200) == full.grid->GridLeft(Width(3) + 200));
}

TEST_CASE("A column grid honours the page's placement for that many columns") {
  nav_test::App();
  Grid grid({100, 100, 100});
  grid.grid->Place(grid.cards[0], 2, 1);
  grid.grid->Place(grid.cards[1], 2, 0);
  grid.grid->Place(grid.cards[2], 2, 1);
  grid.Lay(Width(2));
  CHECK(grid.cards[0]->x() > grid.cards[1]->x());
  CHECK(grid.cards[2]->x() == grid.cards[0]->x());
  CHECK(grid.cards[2]->y() > grid.cards[0]->y());

  // Three columns weren't placed, so they flow.
  grid.Lay(Width(3));
  CHECK(grid.cards[0]->x() < grid.cards[1]->x());
  CHECK(grid.cards[1]->x() < grid.cards[2]->x());

  // A placed column with nothing showing closes up.
  grid.cards[1]->hide();
  CHECK(grid.grid->Columns(Width(2)) == 1);
}

TEST_CASE("A column grid's headers start at its left edge over the cards") {
  nav_test::App();
  Grid grid({100, 100});
  auto* title = new QWidget(&grid.host);
  title->setFixedHeight(30);
  grid.grid->AddHeader(title);
  grid.Lay(Width(3));
  CHECK(title->x() == grid.cards[0]->x());
  CHECK(title->width() == Width(2));
  CHECK(grid.cards[0]->y() > title->geometry().bottom());
  CHECK(grid.cards[1]->y() == grid.cards[0]->y());
}

TEST_CASE("A flowed card that grows stays in its column until the grid reflows") {
  nav_test::App();
  Grid grid({100, 100, 100, 100});
  grid.Lay(Width(2));
  const int second_column = grid.cards[2]->x();
  CHECK(grid.cards[1]->x() < second_column);

  grid.cards[0]->setFixedHeight(400);  // as a card unfolding
  grid.Lay(Width(2));
  CHECK(grid.cards[1]->x() < second_column);
  CHECK(grid.cards[1]->y() > grid.cards[0]->geometry().bottom());

  grid.grid->Reflow();
  grid.Lay(Width(2));
  CHECK(grid.cards[1]->x() == second_column);
}
