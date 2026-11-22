#pragma once

#include <QColor>
#include <QRectF>

class QPainter;

// Shapes drawn with QPainter alone, shared by the desktop components and big screen. Colors and
// sizes come in as arguments, so each side passes its own (tokens, or big screen's units).
namespace mira_gui::paint {

// A chip's or badge's corner for its `height`: a full pill at the default rounding, square at 0,
// and in between scaled with `radius_control` (the theme's rounding for a 32 px control).
qreal PillRadius(qreal height, int radius_control);

// A rounded rect filled with `fill` and, unless `border` is invalid, outlined 1 px inside `rect`.
void Pill(QPainter& painter, const QRectF& rect, qreal radius, const QColor& fill,
          const QColor& border = {});

}  // namespace mira_gui::paint
