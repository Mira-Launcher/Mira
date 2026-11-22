#include "Pill.h"

#include <QPainter>
#include <algorithm>

namespace mira_gui::paint {

qreal PillRadius(qreal height, int radius_control) {
  return std::min(height / 2, radius_control * height / 12);
}

void Pill(QPainter& painter, const QRectF& rect, qreal radius, const QColor& fill,
          const QColor& border) {
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing);
  if (border.isValid()) {
    const QRectF inside = rect.adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(border, 1));
    painter.setBrush(fill.isValid() ? QBrush(fill) : Qt::NoBrush);
    painter.drawRoundedRect(inside, std::max<qreal>(0, radius - 0.5),
                            std::max<qreal>(0, radius - 0.5));
  } else {
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(rect, radius, radius);
  }
  painter.restore();
}

}  // namespace mira_gui::paint
