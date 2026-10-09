#include "ProgressRail.h"

#include <algorithm>
#include <cmath>

#include <QDateTime>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include "../theme/Theme.h"

namespace mira_gui {
namespace {

constexpr int kHeight = 5;
constexpr int kBusyPeriodMs = 1300;
constexpr double kBusyWidth = 0.32;  // of the track

}  // namespace

void PaintRail(QPainter& painter, const QRectF& track, double fraction, const QColor& fill, const QColor& groove) {
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  const qreal radius = track.height() / 2;
  QPainterPath clip;
  clip.addRoundedRect(track, radius, radius);
  painter.setClipPath(clip, Qt::IntersectClip);
  painter.fillRect(track, groove);

  QRectF filled = track;
  if (fraction < 0) {
    // Slides in from the left edge and out past the right, then again.
    const double phase = static_cast<double>(QDateTime::currentMSecsSinceEpoch() % kBusyPeriodMs) / kBusyPeriodMs;
    const double eased = (1 - std::cos(phase * M_PI)) / 2;
    filled.setWidth(track.width() * kBusyWidth);
    filled.moveLeft(track.left() - filled.width() + eased * (track.width() + filled.width()));
  } else {
    filled.setWidth(track.width() * std::clamp(fraction, 0.0, 1.0));
  }
  painter.setPen(Qt::NoPen);
  painter.setBrush(fill);
  painter.drawRoundedRect(filled, radius, radius);
  painter.restore();
}

ProgressRail::ProgressRail(QWidget* parent) : QWidget(parent), tick_(new QTimer(this)) {
  setFixedHeight(kHeight);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  tick_->setInterval(33);
  connect(tick_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
  SetProgress(-1);
}

void ProgressRail::SetProgress(double fraction) {
  fraction_ = fraction;
  if (fraction < 0) {
    tick_->start();
  } else {
    tick_->stop();
  }
  update();
}

void ProgressRail::SetFaint(bool faint) {
  faint_ = faint;
  update();
}

QSize ProgressRail::sizeHint() const { return {90, kHeight}; }

void ProgressRail::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  const theme::Tokens& tokens = theme::Current();
  QColor fill = tokens.accent;
  if (faint_) fill.setAlphaF(0.55);
  PaintRail(painter, QRectF(rect()), fraction_, fill, faint_ ? QColor(Qt::transparent) : tokens.border);
}

}  // namespace mira_gui
