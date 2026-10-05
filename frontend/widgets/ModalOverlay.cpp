#include "ModalOverlay.h"

#include <QMouseEvent>
#include <QPainter>
#include <QRegion>

namespace mira_gui {

ModalOverlay::ModalOverlay(QWidget* parent) : QWidget(parent) { setAttribute(Qt::WA_StyledBackground, true); }

void ModalOverlay::paintEvent(QPaintEvent* event) {
  if (!scrim.isValid()) return QWidget::paintEvent(event);
  QPainter painter(this);
  QRegion region(rect());
  if (clear) region -= clear();
  painter.setClipRegion(region);
  painter.fillRect(rect(), scrim);
}

// A press on the card's own empty space propagates up to here too, so
// only one that lands on no child at all counts as the backdrop.
void ModalOverlay::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !on_backdrop_clicked) return;
  if (childAt(event->position().toPoint()) != nullptr) return;
  on_backdrop_clicked();
}

}  // namespace mira_gui
