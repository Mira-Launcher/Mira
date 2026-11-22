#include "IconButton.h"

namespace mira_gui {

IconButton::IconButton(icons::Glyph glyph, const QString& tip, QWidget* parent, icons::Role role)
    : QToolButton(parent) {
  setAutoRaise(true);
  icons::Follow(this, glyph, role);
  setToolTip(tip);
  setAccessibleName(tip);
}

}  // namespace mira_gui
