#include "Labels.h"

#include <QLabel>
#include <QLocale>

namespace mira_gui {

QString SizeText(qint64 bytes) {
  return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QLabel* MakeLabel(QWidget* parent, const QString& text, const char* role, bool wrap) {
  auto* label = new QLabel(text, parent);
  label->setWordWrap(wrap);
  if (role != nullptr) label->setProperty("role", role);
  return label;
}

QLabel* MakeGroupHeading(QWidget* parent, const QString& text) {
  return MakeLabel(parent, text, "group_heading", /*wrap=*/false);
}

QWidget* MakeDivider(QWidget* parent, Qt::Orientation orientation, int length) {
  auto* divider = new QWidget(parent);
  // Painted from base.qss, so a theme change recolors it with everything else.
  divider->setObjectName("divider");
  divider->setAttribute(Qt::WA_StyledBackground);
  if (orientation == Qt::Horizontal) {
    divider->setFixedHeight(1);
    if (length > 0) divider->setFixedWidth(length);
  } else {
    divider->setFixedWidth(1);
    if (length > 0) divider->setFixedHeight(length);
  }
  return divider;
}

}  // namespace mira_gui
