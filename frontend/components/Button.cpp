#include "Button.h"

#include "../theme/Theme.h"

namespace mira_gui {
namespace {

const char* KindName(ButtonKind kind) {
  switch (kind) {
    case ButtonKind::Secondary:
      return "";
    case ButtonKind::Primary:
      return "primary";
    case ButtonKind::Danger:
      return "danger";
    case ButtonKind::Link:
      return "link";
    case ButtonKind::Text:
      return "text";
    case ButtonKind::Glass:
      return "glass";
  }
  return "";
}

}  // namespace

Button::Button(const QString& text, ButtonKind kind, QWidget* parent) : QPushButton(text, parent) {
  SetKind(kind);
}

void Button::SetKind(ButtonKind kind) {
  kind_ = kind;
  theme::SetStyleProperty(this, "kind", KindName(kind));
}

}  // namespace mira_gui
