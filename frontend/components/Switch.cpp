#include "Switch.h"

#include <QAbstractButton>
#include <QPainter>
#include <QVariantAnimation>
#include <QWidget>

#include "../theme/Theme.h"

namespace mira_gui {

Switch::Switch(QWidget* parent) : QAbstractButton(parent) {
  setCheckable(true);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::StrongFocus);
  slide_ = new QVariantAnimation(this);
  slide_->setDuration(120);
  connect(slide_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    position_ = value.toReal();
    update();
  });
  connect(this, &QAbstractButton::toggled, this, [this](bool on) {
    slide_->stop();
    if (!isVisible()) {
      position_ = on ? 1 : 0;
      update();
      return;
    }
    slide_->setStartValue(position_);
    slide_->setEndValue(on ? 1.0 : 0.0);
    slide_->start();
  });
}

QSize Switch::sizeHint() const {
  return {36, 22};
}

void Switch::paintEvent(QPaintEvent*) {
  // Set with its signals blocked, the switch never saw the toggle that starts the slide.
  if (slide_->state() != QAbstractAnimation::Running) position_ = isChecked() ? 1 : 0;
  const theme::Tokens& tokens = theme::Current();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QRectF track = QRectF(0, 0, 34, 20).translated(1, (height() - 20) / 2.0);
  const QColor off = tokens.surface_alt;
  const QColor on = isEnabled() ? tokens.accent : tokens.border;
  painter.setPen(QPen(position_ > 0.5 ? on : tokens.border, 1));
  // The track color follows the knob's travel, so the animation carries the color too.
  painter.setBrush(theme::Mix(off, on, position_));
  painter.drawRoundedRect(track.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);

  const qreal travel = track.width() - 18;
  const QRectF knob(track.left() + 3 + travel * position_, track.top() + 3, 14, 14);
  painter.setPen(Qt::NoPen);
  painter.setBrush(position_ > 0.5 ? tokens.on_accent : tokens.text_muted);
  painter.drawEllipse(knob);
}

}  // namespace mira_gui
