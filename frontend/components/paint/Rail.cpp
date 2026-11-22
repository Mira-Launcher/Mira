#include "Rail.h"

#include <QDateTime>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace mira_gui::paint {
namespace {

constexpr int kBusyPeriodMs = 1300;
constexpr double kBusyWidth = 0.32;  // of the track

}  // namespace

void Rail(QPainter& painter, const QRectF& track, double fraction, const QColor& fill,
          const QColor& groove) {
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
    const double phase =
        static_cast<double>(QDateTime::currentMSecsSinceEpoch() % kBusyPeriodMs) / kBusyPeriodMs;
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

}  // namespace mira_gui::paint
