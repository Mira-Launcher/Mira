#pragma once

#include <QColor>
#include <QRectF>

class QPainter;

namespace mira_gui::paint {

// Mira's one progress look: a thin rounded rail. `fraction` is 0..1, or below 0 when nothing
// reports how far along it is, which draws a short segment sliding along, placed by the clock so
// every rail on screen moves together.
void Rail(QPainter& painter, const QRectF& track, double fraction, const QColor& fill,
          const QColor& groove);

}  // namespace mira_gui::paint
