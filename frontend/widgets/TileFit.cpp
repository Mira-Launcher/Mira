#include "TileFit.h"

#include <cstdlib>
#include <optional>

namespace mira_gui {

int FitTileWidth(int width, int from, TileRow row, int min, int max) {
  const int step = (width > from) - (width < from);
  std::optional<int> best;
  bool fits = false;
  const auto extent = [&row](int tile) { return row.rows ? tile * 3 / 2 : tile; };
  for (int count = 1; count * extent(min) <= row.room; ++count) {
    const int span = (row.room + row.gap) / count - row.gap;
    const int fit = row.rows ? span * 2 / 3 : span;
    if (fit < min || fit > max) continue;
    fits = true;
    if (step > 0   ? fit > from && (!best || fit < *best)
        : step < 0 ? fit < from && (!best || fit > *best)
                   : !best || std::abs(fit - width) < std::abs(*best - width)) {
      best = fit;
    }
  }
  if (best) return *best;
  return fits && step != 0 ? from : width;
}

int TileHeight(int width, TileRow row) {
  if (row.rows && width > 0) {
    for (int count = 1; count * width <= row.room; ++count) {
      const int span = (row.room + row.gap) / count - row.gap;
      if (span * 2 / 3 == width) return span;
    }
  }
  return width * 3 / 2;
}

}  // namespace mira_gui
