#include "Popover.h"

namespace mira_gui {

Popover::Popover(QWidget* parent) : QWidget(parent, Qt::Popup) {
  setAttribute(Qt::WA_StyledBackground);
}

void Popover::PopUpBelow(QWidget* anchor, popup::Align align) {
  adjustSize();
  const QRect global(anchor->mapToGlobal(QPoint(0, 0)), anchor->size());
  move(popup::PlaceBelow(global, size(), align, kGap));
  show();
}

}  // namespace mira_gui
