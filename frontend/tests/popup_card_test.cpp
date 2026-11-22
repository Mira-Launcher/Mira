#include <doctest.h>

#include <QRect>

#include "components/PopupCard.h"

using namespace mira_gui;

TEST_CASE("A popover drops from its anchor and stays on screen") {
  const QRect screen(0, 0, 1366, 768);
  const QSize size(380, 300);
  const int gap = 4;

  SUBCASE("End-aligned under a top bar button, its right edge on the button's") {
    const QRect button(1200, 8, 32, 32);
    const QRect at(popup::PlaceBelow(button, size, popup::Align::End, gap, screen), size);
    CHECK(at.right() == button.right());
    CHECK(at.top() == button.bottom() + 1 + gap);
  }

  SUBCASE("Start-aligned under a pill, its left edge on the pill's") {
    const QRect pill(400, 60, 160, 30);
    const QRect at(popup::PlaceBelow(pill, size, popup::Align::Start, gap, screen), size);
    CHECK(at.left() == pill.left());
    CHECK(at.top() == pill.bottom() + 1 + gap);
  }

  SUBCASE("With no room below it opens above its anchor") {
    const QRect pill(400, 700, 160, 30);
    const QRect at(popup::PlaceBelow(pill, size, popup::Align::Start, gap, screen), size);
    CHECK(at.bottom() < pill.top());
  }

  SUBCASE("An anchor near the left edge doesn't push it off screen") {
    const QRect button(10, 8, 32, 32);
    const QRect at(popup::PlaceBelow(button, size, popup::Align::End, gap, screen), size);
    CHECK(screen.contains(at));
  }
}
