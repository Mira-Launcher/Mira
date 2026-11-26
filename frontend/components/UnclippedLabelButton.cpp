#include "UnclippedLabelButton.h"

#include <QStyleOptionButton>
#include <QStylePainter>

namespace mira_gui {

void UnclippedLabelButton::paintEvent(QPaintEvent* /*event*/) {
  QStylePainter painter(this);
  QStyleOptionButton option;
  initStyleOption(&option);
  painter.drawControl(QStyle::CE_PushButtonBevel, option);
  if (option.state & QStyle::State_HasFocus) {
    QStyleOptionFocusRect focus;
    focus.QStyleOption::operator=(option);
    focus.rect = style()->subElementRect(QStyle::SE_PushButtonFocusRect, &option, this);
    painter.drawPrimitive(QStyle::PE_FrameFocusRect, focus);
  }
  // The style takes the padding off whatever rect it's given; one a width wider on each side keeps
  // the label centered with only the button's own edges to stop it.
  QStyleOptionButton label = option;
  label.rect = rect().adjusted(-width(), 0, width(), 0);
  painter.drawControl(QStyle::CE_PushButtonLabel, label);
}

}  // namespace mira_gui
