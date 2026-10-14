#include <doctest.h>

#include <algorithm>
#include <vector>

#include "widgets/TileFit.h"

using namespace mira_gui;

namespace {

constexpr int kMin = 120;
constexpr int kMax = 260;
constexpr int kStep = 10;  // the zoom slider's page step

// The widths a run of Ctrl+plus (`dir` 1) or Ctrl+minus (-1) presses lands on, from `start`.
std::vector<int> Presses(TileRow row, int start, int dir, int count) {
  std::vector<int> widths;
  int value = start;
  for (int i = 0; i < count; ++i) {
    const int width = std::clamp(value + dir * kStep, kMin, kMax);
    value = FitTileWidth(width, value, row, kMin, kMax);
    widths.push_back(value);
  }
  return widths;
}

int PerRow(TileRow row, int width) {
  return (row.room + row.gap) / (width + row.gap);
}

}  // namespace

TEST_CASE(
    "Zoom steps change the column count each press and stop at the last width that fills the row") {
  for (const TileRow row : {TileRow{2113, 0}, TileRow{1180, 0}, TileRow{1500, 12}}) {
    CAPTURE(row.room);
    CAPTURE(row.gap);
    const int smallest = FitTileWidth(kMin, kMin, row, kMin, kMax);
    const int largest = FitTileWidth(kMax, kMax, row, kMin, kMax);

    const std::vector<int> in = Presses(row, smallest, 1, 30);
    for (std::size_t i = 1; i < in.size() && in[i] != in[i - 1]; ++i)
      CHECK(PerRow(row, in[i]) == PerRow(row, in[i - 1]) - 1);
    CHECK(in.back() == largest);
    CHECK((row.room + row.gap) % (in.back() + row.gap) < PerRow(row, in.back()));

    const std::vector<int> out = Presses(row, largest, -1, 30);
    for (std::size_t i = 1; i < out.size() && out[i] != out[i - 1]; ++i)
      CHECK(PerRow(row, out[i]) == PerRow(row, out[i - 1]) + 1);
    CHECK(out.back() == smallest);
    CHECK((row.room + row.gap) % (out.back() + row.gap) < PerRow(row, out.back()));
  }
}

TEST_CASE("A released zoom slider snaps to the nearest width that fills the row") {
  const TileRow row{2113, 0};
  CHECK(FitTileWidth(200, 200, row, kMin, kMax) == 2113 / 11);
  // A row narrower than the smallest tile has no fit: the width is kept.
  CHECK(FitTileWidth(150, 160, TileRow{100, 0}, kMin, kMax) == 150);
}
