#include "components/Segmented.h"

#include <doctest.h>

#include <QFontMetrics>
#include <QImage>
#include <QPushButton>

#include "NavWindow.h"

using namespace mira_gui;

namespace {

// How many columns from the first dark pixel to the last.
int InkWidth(QWidget* widget) {
  QImage image(widget->size(), QImage::Format_ARGB32);
  image.fill(Qt::white);
  widget->render(&image);
  int first = image.width(), last = -1;
  for (int x = 0; x < image.width(); ++x) {
    for (int y = 0; y < image.height(); ++y) {
      if (QColor(image.pixel(x, y)).lightness() < 128) {
        first = std::min(first, x);
        last = std::max(last, x);
      }
    }
  }
  return last - first + 1;
}

}  // namespace

TEST_CASE("A checked segment's semibold text shows in full in the size its regular text set") {
  nav_test::App();
  Segmented segmented({{"auto", "System's choice"}, {"on", "On"}}, nullptr);
  // base.qss's look: padding round the text, and semibold once checked. The font goes in the sheet
  // because a styled child takes the app's font, not its parent's.
  segmented.setStyleSheet(
      "QPushButton { font-family: 'DejaVu Sans'; font-size: 10pt; background: white; color: black;"
      " border: none; padding: 5px 18px; }"
      "QPushButton:checked { font-weight: 600; }");
  QPushButton* option = segmented.Option("auto");
  option->ensurePolished();
  option->resize(option->sizeHint());
  QFont bold = option->font();
  bold.setWeight(QFont::DemiBold);
  const int regular = option->fontMetrics().horizontalAdvance(option->text());
  const int semibold = QFontMetrics(bold).horizontalAdvance(option->text());
  REQUIRE(semibold > regular + 4);  // a font whose bold is wider, or the check can't fail

  const int unchecked_width = option->width();
  segmented.SetValue("auto");
  CHECK(option->sizeHint().width() == unchecked_width);
  // Ink runs a pixel or two inside the advance at each end.
  CHECK(InkWidth(option) >= semibold - 4);
}
