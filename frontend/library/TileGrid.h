#pragma once

#include <QSize>

#include <functional>

#include "TileView.h"

namespace mira_gui {

class ArtworkStore;
class GameTileDelegate;

// A wrapping grid of cover tiles over any model, that grows to fit them
// instead of scrolling, for a page that scrolls as a whole. Tiles are
// painted by GameTileDelegate from the model's roles.
class TileGrid : public TileView {
public:
  TileGrid(QSize tile, ArtworkStore* artwork, QWidget* parent = nullptr);

  // A tile's ActionRole pill was clicked (only while ActionEnabledRole).
  std::function<void(const QModelIndex&)> on_action;
  // Ctrl+wheel: one call per notch, positive to grow. The rest of the wheel
  // scrolls the page around the grid.
  std::function<void(int steps)> on_ctrl_wheel;

  // New tile size; the covers redraw at it.
  void SetTileSize(QSize tile);
  // Call after rows are added, removed or hidden.
  void FitHeight();
  int VisibleCount() const;

protected:
  void resizeEvent(QResizeEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  // Left to the page's own scroll area.
  void wheelEvent(QWheelEvent* event) override;
  void rowsInserted(const QModelIndex& parent, int start, int end) override;
  void rowsAboutToBeRemoved(const QModelIndex& parent, int start, int end) override;

private:
  QModelIndex ActionIndexAt(const QPoint& pos) const;
};

}  // namespace mira_gui
