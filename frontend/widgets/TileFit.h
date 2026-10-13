#pragma once

namespace mira_gui {

// The width a row of tiles has, and the gap between them, for snapping the tile size.
struct TileRow {
  int room = 0;
  int gap = 0;
};
// The width in [min, max] whose tiles fill `row` exactly: the next one past `from` toward
// `width`, or the nearest when they're equal. `from` when none lies that way, so a zoom past the
// last fit stays put rather than leaving a gap; `width` when none fits at all.
int FitTileWidth(int width, int from, TileRow row, int min, int max);

}  // namespace mira_gui
