#include "TileGrid.h"

#include <algorithm>

#include <QMouseEvent>
#include <QTimer>
#include <QWheelEvent>

#include "ArtworkStore.h"
#include "GameTileDelegate.h"

namespace mira_gui {

TileGrid::TileGrid(QSize tile, ArtworkStore* artwork, QWidget* parent) : TileView(parent), artwork_(artwork) {
  setItemDelegate(new GameTileDelegate(this, tile, artwork));
  if (artwork != nullptr) connect(artwork, &ArtworkStore::RequestsDropped, viewport(), qOverload<>(&QWidget::update));
  setViewMode(QListView::IconMode);
  setResizeMode(QListView::Adjust);
  setMovement(QListView::Static);
  setUniformItemSizes(true);
  setSpacing(0);
  setGridSize(tile);
  setEditTriggers(QAbstractItemView::NoEditTriggers);
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  viewport()->setAutoFillBackground(false);
  setStyleSheet("QListView { background: transparent; border: none; }");
}

void TileGrid::paintEvent(QPaintEvent* event) {
  TileView::paintEvent(event);
  // Store titles' covers for the tiles just below the screen, so scrolling finds them drawn.
  constexpr int kAhead = 50;
  if (artwork_ == nullptr || model() == nullptr) return;
  const QRect shown = viewport()->visibleRegion().boundingRect();
  if (shown.isEmpty()) return;
  const QModelIndex last = indexAt(QPoint(shown.left() + 1, shown.bottom()));
  if (!last.isValid()) return;
  const QSize tile = static_cast<GameTileDelegate*>(itemDelegate())->TileSize();
  const int end = std::min(model()->rowCount(), last.row() + kAhead);
  for (int row = last.row(); row < end; ++row) {
    const QStringList title = model()->index(row, 0).data(GameTileDelegate::TitleCoverRole).toStringList();
    if (title.size() == 2) artwork_->PrefetchTitle(title[0], title[1], tile, devicePixelRatioF());
  }
}

void TileGrid::SetTileSize(QSize tile) {
  static_cast<GameTileDelegate*>(itemDelegate())->SetTileSize(tile);
  setGridSize(tile);
  FitHeight();
  viewport()->update();
}

int TileGrid::VisibleCount() const {
  if (model() == nullptr) return 0;
  int visible = 0;
  for (int row = 0; row < model()->rowCount(); ++row) {
    if (!isRowHidden(row)) ++visible;
  }
  return visible;
}

void TileGrid::FitHeight() {
  const QSize cell = gridSize();
  const int per_row = qMax(1, viewport()->width() / qMax(1, cell.width()));
  const int rows = (VisibleCount() + per_row - 1) / per_row;
  setFixedHeight(rows * cell.height() + 2 * frameWidth());
}

void TileGrid::resizeEvent(QResizeEvent* event) {
  QListView::resizeEvent(event);
  FitHeight();
}

// Deferred: the model's own count isn't final until the insert or removal has run.
void TileGrid::rowsInserted(const QModelIndex& parent, int start, int end) {
  QListView::rowsInserted(parent, start, end);
  QTimer::singleShot(0, this, &TileGrid::FitHeight);
}

void TileGrid::rowsAboutToBeRemoved(const QModelIndex& parent, int start, int end) {
  QListView::rowsAboutToBeRemoved(parent, start, end);
  QTimer::singleShot(0, this, &TileGrid::FitHeight);
}

QModelIndex TileGrid::ActionIndexAt(const QPoint& pos) const {
  const QModelIndex hit = indexAt(pos);
  if (!hit.isValid() || !hit.data(GameTileDelegate::ActionEnabledRole).toBool()) return {};
  return GameTileDelegate::ActionRect(visualRect(hit), hit, font()).contains(pos) ? hit : QModelIndex();
}

void TileGrid::mouseMoveEvent(QMouseEvent* event) {
  viewport()->setCursor(ActionIndexAt(event->pos()).isValid() ? Qt::PointingHandCursor : Qt::ArrowCursor);
  TileView::mouseMoveEvent(event);
}

void TileGrid::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && on_action) {
    if (const QModelIndex hit = ActionIndexAt(event->pos()); hit.isValid()) {
      on_action(hit);
      return;
    }
  }
  TileView::mouseReleaseEvent(event);
}

void TileGrid::wheelEvent(QWheelEvent* event) {
  if ((event->modifiers() & Qt::ControlModifier) && on_ctrl_wheel) {
    if (const int steps = event->angleDelta().y() / 120; steps != 0) on_ctrl_wheel(steps);
    event->accept();
    return;
  }
  StopHover();  // the tile is about to scroll out from under its card
  event->ignore();
}

}  // namespace mira_gui
