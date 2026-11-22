#include "components/Chip.h"

#include <doctest.h>

#include <QTest>
#include <cmath>

#include "NavWindow.h"
#include "components/ChipGroup.h"
#include "components/Variants.h"
#include "library/GamePresentation.h"
#include "library/TitleRatings.h"
#include "theme/Theme.h"

using namespace mira_gui;

namespace {

double Luminance(const QColor& color) {
  const auto linear = [](double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) +
         0.0722 * linear(color.blueF());
}

// WCAG's contrast ratio, 1 to 21.
double Contrast(const QColor& a, const QColor& b) {
  const double x = Luminance(a), y = Luminance(b);
  return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}

}  // namespace

TEST_CASE("A removable chip's × or Delete removes it and a click elsewhere on it is a click") {
  nav_test::App();
  Chip chip("Celeste");
  chip.SetRemovable(true, "Remove Celeste");
  chip.resize(chip.sizeHint());
  chip.show();
  int clicks = 0, removes = 0;
  QObject::connect(&chip, &Chip::clicked, [&clicks] { ++clicks; });
  QObject::connect(&chip, &Chip::RemoveClicked, [&removes] { ++removes; });

  // The × is the chip's last glyph, just inside its right padding.
  const QPoint cross(chip.width() - chip.height() / 2 - 2, chip.height() / 2);
  QTest::mouseClick(&chip, Qt::LeftButton, {}, cross);
  CHECK(removes == 1);
  CHECK(clicks == 0);

  QTest::mouseClick(&chip, Qt::LeftButton, {}, QPoint(chip.height() / 2, chip.height() / 2));
  CHECK(clicks == 1);
  CHECK(removes == 1);

  // From the keyboard, Delete and Backspace remove it; Space is still a click.
  QTest::keyClick(&chip, Qt::Key_Delete);
  QTest::keyClick(&chip, Qt::Key_Backspace);
  CHECK(removes == 3);
  QTest::keyClick(&chip, Qt::Key_Space);
  CHECK(clicks == 2);
}

TEST_CASE("A chip group keeps values it doesn't offer and greys a chip without its pair") {
  nav_test::App();
  ChipGroup group;
  group.SetChoices(
      {{"mangohud", "MangoHud", {}, {}, false}, {"gamescope_hdr", "HDR", {}, "gamescope", true}});
  group.SetValues({"mangohud", "unknown_step"});
  CHECK(group.Values().contains("mangohud"));
  CHECK(group.Values().contains("unknown_step"));

  group.SetPairValues({});
  const QList<Chip*> chips = group.findChildren<Chip*>();
  REQUIRE(chips.size() == 2);
  CHECK(chips[0]->isEnabled());
  CHECK_FALSE(chips[1]->isEnabled());
  group.SetPairValues({"gamescope"});
  CHECK(chips[1]->isEnabled());
}

TEST_CASE("Tier and review colors read as text on either theme and under text on their fill") {
  nav_test::App();
  std::vector<QColor> colors;
  for (const char* tier : kProtonDbFilterTiers) {
    if (*tier != '\0') colors.push_back(ProtonDbTierColor(tier));
  }
  for (const ReviewBucket bucket : kReviewBuckets) {
    if (ReviewBucketColor(bucket).isValid()) colors.push_back(ReviewBucketColor(bucket));
  }
  for (const char* theme_name : {"mira-dark", "mira-light"}) {
    theme::Apply(theme_name);
    const QColor surface = theme::Current().surface_alt;
    for (const QColor& color : colors) {
      CAPTURE(theme_name);
      CAPTURE(color.name().toStdString());
      CHECK(Contrast(InkFor(color), surface) >= 3.0);
      CHECK(Contrast(InkOn(color), color) >= 4.0);
    }
  }
}
