#pragma once

namespace mira_gui {

// The width a row of tiles has, and the gap between them, for snapping the tile size. With
// `rows`, `room` is instead the height whole rows of tiles fill, each TileHeight tall.
struct TileRow {
  int room = 0;
  int gap = 0;
  bool rows = false;
};
// The width in [min, max] whose tiles fill `row` exactly: the next one past `from` toward
// `width`, or the nearest when they're equal. `from` when none lies that way, so a zoom past the
// last fit stays put rather than leaving a gap; `width` when none fits at all.
int FitTileWidth(int width, int from, TileRow row, int min, int max);
// A tile's height: 3/2 its width, or for a width that fills `row.rows`, the rows' exact share of
// the room. Rounding both ways would leave up to 3 px a row, enough to show the next row's covers.
int TileHeight(int width, TileRow row);

}  // namespace mira_gui
