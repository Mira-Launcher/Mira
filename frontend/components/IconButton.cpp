#include "IconButton.h"

#include <QPainter>

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

void IconButton::SetSpinning(bool spinning) {
  if (spin_ == nullptr) {
    spin_ = new QVariantAnimation(this);
    spin_->setStartValue(0.0);
    spin_->setEndValue(360.0);
    spin_->setDuration(800);
    connect(spin_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) { ApplyAngle(value.toReal()); });
    connect(spin_, &QVariantAnimation::finished, this, &IconButton::Apply);
  }
  if (spinning) {
    spin_->setLoopCount(-1);
    if (spin_->state() != QAbstractAnimation::Running) spin_->start();
  } else if (spin_->state() == QAbstractAnimation::Running) {
    // Ends at the end of this turn, upright.
    spin_->setLoopCount(spin_->currentLoop() + 1);
  }
}

void IconButton::ApplyAngle(qreal degrees) {
  const qreal ratio = devicePixelRatioF();
  const QSize size = iconSize();
  const QPixmap glyph = icons::For(glyph_, theme::Current().*role_).pixmap(size, ratio);
  QPixmap turned(glyph.size());
  turned.setDevicePixelRatio(ratio);
  turned.fill(Qt::transparent);
  QPainter painter(&turned);
  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  const QPointF centre(size.width() / 2.0, size.height() / 2.0);
  painter.translate(centre);
  painter.rotate(degrees);
  painter.translate(-centre);
  painter.drawPixmap(0, 0, glyph);
  painter.end();
  setIcon(QIcon(turned));
}

}  // namespace mira_gui
