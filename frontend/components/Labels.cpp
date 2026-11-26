#include "Labels.h"

#include <QLabel>
#include <QPainter>

namespace mira_gui {

QLabel* MakeLabel(QWidget* parent, const QString& text, TextStyle style, bool wrap) {
  auto* label = new QLabel(text, parent);
  label->setWordWrap(wrap);
  SetTextStyle(label, style);
  return label;
}

QLabel* MakeGroupHeading(QWidget* parent, const QString& text) {
  return MakeLabel(parent, text, text::kGroup, /*wrap=*/false);
}

void KeepSpaceWhenHidden(QWidget* widget, bool keep) {
  QSizePolicy policy = widget->sizePolicy();
  policy.setRetainSizeWhenHidden(keep);
  widget->setSizePolicy(policy);
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

QSize ElidedLabel::minimumSizeHint() const {
  return QSize(20, QLabel::minimumSizeHint().height());
}

void ElidedLabel::paintEvent(QPaintEvent*) {
  const QString shown = fontMetrics().elidedText(text(), Qt::ElideRight, width());
  // Here rather than on resize, since setText isn't virtual.
  setToolTip(shown == text() ? QString() : text());
  QPainter painter(this);
  painter.setPen(palette().color(QPalette::WindowText));
  painter.drawText(rect(), alignment(), shown);
}

WrapWhenNarrowLabel::WrapWhenNarrowLabel(const QString& text, QWidget* parent)
    : QLabel(text, parent) {
  setWordWrap(true);
}

QSize WrapWhenNarrowLabel::sizeHint() const {
  const QMargins margins = contentsMargins();
  // Measured as QLabel lays out wrapped text: horizontalAdvance can come out a pixel short of
  // that, and the label then wraps its last word at its own width.
  const int text_width =
      fontMetrics().boundingRect(QRect(0, 0, 100000, 1000), Qt::TextWordWrap, text()).width();
  const int width = text_width + 2 * margin() + margins.left() + margins.right() + 1;
  return {width, heightForWidth(width)};
}

}  // namespace mira_gui
