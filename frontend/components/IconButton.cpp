#include "IconButton.h"

namespace mira_gui {

IconButton::IconButton(icons::Glyph glyph, const QString& tip, QWidget* parent, icons::Role role)
    : QToolButton(parent), glyph_(glyph), role_(role) {
  setAutoRaise(true);
  Apply();
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, &IconButton::Apply);
  setToolTip(tip);
  setAccessibleName(tip);
}

void IconButton::SetGlyph(icons::Glyph glyph) {
  glyph_ = glyph;
  Apply();
}

void IconButton::Apply() {
  setIcon(icons::For(glyph_, theme::Current().*role_));
}

}  // namespace mira_gui
