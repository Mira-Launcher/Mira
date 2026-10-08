#include "Labels.h"

#include <QLabel>
#include <QLocale>
#include <QPainter>

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

ElidedLabel::ElidedLabel(const QString& text, QWidget* parent) : QLabel(text, parent) {}

QSize ElidedLabel::minimumSizeHint() const { return QSize(20, QLabel::minimumSizeHint().height()); }

void ElidedLabel::paintEvent(QPaintEvent*) {
  const QString shown = fontMetrics().elidedText(text(), Qt::ElideRight, width());
  // Here rather than on resize, since setText isn't virtual.
  setToolTip(shown == text() ? QString() : text());
  QPainter painter(this);
  painter.setPen(palette().color(QPalette::WindowText));
  painter.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, shown);
}

QString StatusDot(const QColor& color) {
  return QString("<span style='color:%1'>●</span> ").arg(color.name());
}

}  // namespace mira_gui
