#include "Popover.h"

namespace mira_gui {

Popover::Popover(QWidget* parent) : QWidget(parent, Qt::Popup) {
  setAttribute(Qt::WA_StyledBackground);
}

void Popover::PopUpBelow(QWidget* anchor, popup::Align align) {
  anchor_ = anchor;
  align_ = align;
  adjustSize();
  Place();
  show();
}

void Popover::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  if (isVisible()) Place();
}

void Popover::Place() {
  if (anchor_ != nullptr) move(popup::PlaceBelow(anchor_, size(), align_, kGap));
}

}  // namespace mira_gui
