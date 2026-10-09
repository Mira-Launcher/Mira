#pragma once

#include <QItemSelection>
#include <QListView>

#include <functional>

#include "../library/HoverCard.h"

class QRubberBand;
class QTimer;

namespace mira_gui {

// The width a row of tiles has, and the gap between them, for snapping the tile size.
struct TileRow {
  int room = 0;
  int gap = 0;
};
// The width in [min, max] whose tiles fill `row` exactly: the next one past `from` toward
// `width`, or the nearest when they're equal. `width` when none fits.
int FitTileWidth(int width, int from, TileRow row, int min, int max);
// What's left of `width` for tiles once a scrollbar shows, whether or not one does yet: one
// showing up later then doesn't wrap the last tile of each row.
int RoomBesideScrollbar(const QWidget* widget, int width);

// A grid of cover tiles over any model, with the hover card dwell and
// drag-to-select that every tile grid shares. Tiles fill their whole cell,
// so Qt's own rubber band (empty-space presses only) has nowhere to start;
// this one starts on a tile too. Left-button moves never reach QListView,
// because Qt's own drag-select state survives a swallowed release and then
// draws a second, dead rubber band on the next plain hover.
class TileView : public QListView {
public:
  explicit TileView(QWidget* parent = nullptr);

  // An index after the cursor rests on it, an invalid one once it moves off.
  std::function<void(const QModelIndex&)> on_hover;

  TileRow Row() const { return {RoomBesideScrollbar(this, viewport()->width()), 0}; }
  void SetDragSelectEnabled(bool enabled);
  // A tile painted something that moves (a busy progress rail): repaint soon.
  // Stops by itself once no paint asks again.
  void KeepAnimating();

protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void focusOutEvent(QFocusEvent* event) override;
  void changeEvent(QEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  // The tile is about to scroll out from under its card.
  void StopHover() { hover_.Track(QModelIndex()); }

private:
  // Within this far of the top/bottom edge (or past it), a drag scrolls.
  static constexpr int kAutoScrollEdge = 40;

  // Viewport to content coordinates, so a drag's origin stays put while
  // the grid scrolls under it.
  QPoint Offset() const { return QPoint(horizontalOffset(), verticalOffset()); }
  void UpdateDrag();
  void AutoScrollStep();
  void EndDrag();

  bool drag_select_enabled_ = true;
  bool tracking_drag_ = false;
  QPoint drag_origin_;  // content coordinates
  QPoint drag_pos_;     // viewport coordinates, last seen
  Qt::KeyboardModifiers drag_modifiers_;  // at the press
  QRubberBand* rubber_band_ = nullptr;
  QTimer* autoscroll_timer_ = nullptr;
  QTimer* animation_timer_ = nullptr;
  QItemSelection base_selection_;  // what was selected when the drag began
  HoverDwell hover_{[this](const QModelIndex& index) {
    if (on_hover) on_hover(index);
  }};
};

}  // namespace mira_gui
