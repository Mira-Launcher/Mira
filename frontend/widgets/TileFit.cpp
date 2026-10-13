#include "TileFit.h"

#include <cstdlib>
#include <optional>

namespace mira_gui {

int FitTileWidth(int width, int from, TileRow row, int min, int max) {
  const int step = (width > from) - (width < from);
  std::optional<int> best;
  bool fits = false;
  for (int per_row = 1; per_row * min <= row.room; ++per_row) {
    const int fit = (row.room + row.gap) / per_row - row.gap;
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

}  // namespace mira_gui
