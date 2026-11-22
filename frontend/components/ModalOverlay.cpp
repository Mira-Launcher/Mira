#include "ModalOverlay.h"

#include <QMouseEvent>
#include <QPainter>
#include <QRegion>

#include "../theme/Theme.h"

namespace mira_gui {

ModalOverlay::ModalOverlay(QWidget* parent) : QWidget(parent) {}

void ModalOverlay::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  QRegion region(rect());
  if (clear) region -= clear();
  painter.setClipRegion(region);
  painter.fillRect(rect(), ModalScrim());
}

// A press on the card's own empty space propagates up to here too, so
// only one that lands on no child at all counts as the backdrop.
void ModalOverlay::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !on_backdrop_clicked) return;
  if (childAt(event->position().toPoint()) != nullptr) return;
  on_backdrop_clicked();
}

QColor ModalScrim() {
  return {0, 0, 0, theme::Current().modal_scrim_alpha};
}

}  // namespace mira_gui
