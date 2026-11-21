#include "display/Display.h"

#include <doctest.h>

using namespace mira;

TEST_CASE("The primary monitor, its HDR and modes are read from kscreen-doctor") {
  const std::string text =
      "Output: 2 HDMI-A-1 uuid\n\tenabled\n\tpriority 2\n\tModes:  7:1920x1080@60.00*\n"
      "\tHDR: incapable\n"
      "\x1b[01;32mOutput: \x1b[0;0m1 DP-3 uuid\n\tenabled\n\tpriority 1\n"
      "\tModes:  1:2560x1440@59.95!  2:2560x1440@165.08*  3:2560x1440@144.00  "
      "6:1920x1080@119.88  7:1920x1080@60.00\n"
      "\tHDR: disabled\n\tWide Color Gamut: disabled\n";
  const auto primary = display::ParsePrimary(text);
  REQUIRE(primary);
  CHECK(primary->name == "DP-3");
  CHECK(primary->mode == "2");
  CHECK(primary->hdr == false);
  CHECK(primary->wcg == false);
  CHECK(display::FindMode(*primary, "1920x1080@60") == "7");
  CHECK(display::FindMode(*primary, "2560x1440") == "2");  // the fastest
  CHECK(display::FindMode(*primary, "2560x1440@144") == "3");
  CHECK(display::FindMode(*primary, "1280x720").empty());
  CHECK_FALSE(display::ParsePrimary("Output: 1 DP-1 uuid\n\tpriority 2\n"));
}
