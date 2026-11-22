#include "Text.h"

#include <QStyle>
#include <QWidget>

namespace mira_gui {
namespace {

const char* RoleName(TextRole role) {
  switch (role) {
    case TextRole::Body:
      return "";
    case TextRole::Section:
      return "section";
    case TextRole::Heading:
      return "heading";
    case TextRole::Group:
      return "group";
  }
  return "";
}

}  // namespace

void SetTextStyle(QWidget* widget, TextStyle style) {
  const char* role = RoleName(style.role);
  const char* size = style.size == TextSize::Small ? "small" : "";
  const char* tone = ToneName(style.tone);
  if (widget->property("text_role").toString() == role &&
      widget->property("text_size").toString() == size &&
      widget->property("tone").toString() == tone) {
    return;
  }
  widget->setProperty("text_role", role);
  widget->setProperty("text_size", size);
  widget->setProperty("tone", tone);
  if (widget->testAttribute(Qt::WA_WState_Polished)) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
  }
}

}  // namespace mira_gui
