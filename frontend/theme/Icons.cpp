#include "Icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <QPixmap>

#include <cmath>

#include "Theme.h"

namespace mira_gui::icons {
namespace {

// Everything below is drawn on a 16x16 grid and scaled to whatever size is
// being asked for, so one description covers every DPI.
constexpr qreal kGrid = 16.0;

QPainterPath Hub(qreal center) {
  QPainterPath hub;
  hub.addEllipse(QPointF(center, center), 2.0, 2.0);
  return hub;
}

void PaintGear(QPainter& painter, const QColor& color) {
  constexpr int kTeeth = 8;
  constexpr qreal kCenter = kGrid / 2;

  // A body with teeth laid on top, rather than one polygon alternating
  // between two radii: that construction gives spikes, which at 16px reads as
  // a sunburst rather than a gear.
  QPainterPath gear;
  gear.addEllipse(QPointF(kCenter, kCenter), 4.9, 4.9);
  for (int tooth = 0; tooth < kTeeth; ++tooth) {
    QPainterPath one;
    one.addRoundedRect(QRectF(kCenter - 1.35, kCenter - 7.1, 2.7, 3.4), 0.7, 0.7);
    QTransform rotation;
    rotation.translate(kCenter, kCenter);
    rotation.rotate(tooth * 360.0 / kTeeth);
    rotation.translate(-kCenter, -kCenter);
    gear = gear.united(rotation.map(one));
  }
  gear = gear.subtracted(Hub(kCenter));

  painter.setPen(Qt::NoPen);
  painter.setBrush(color);
  painter.drawPath(gear);
}

void PaintGlyph(QPainter& painter, Glyph glyph, const QColor& color) {
  QPen pen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);

  switch (glyph) {
    case Glyph::Menu:
      for (const qreal y : {4.5, 8.0, 11.5}) painter.drawLine(QPointF(2.5, y), QPointF(13.5, y));
      return;
    case Glyph::Settings:
      PaintGear(painter, color);
      return;
    case Glyph::Minimize:
      painter.drawLine(QPointF(3.5, 8.0), QPointF(12.5, 8.0));
      return;
    case Glyph::Maximize:
      painter.drawRect(QRectF(3.5, 3.5, 9.0, 9.0));
      return;
    case Glyph::Restore:
      // The front window, then the one behind it clipped to an L so the two
      // do not read as a single grid at 16px.
      painter.drawRect(QRectF(3.0, 5.5, 7.5, 7.0));
      painter.drawPolyline(QPolygonF({QPointF(5.5, 5.0), QPointF(5.5, 3.0), QPointF(13.0, 3.0),
                                      QPointF(13.0, 10.0), QPointF(11.0, 10.0)}));
      return;
    case Glyph::Close:
      painter.drawLine(QPointF(4.0, 4.0), QPointF(12.0, 12.0));
      painter.drawLine(QPointF(12.0, 4.0), QPointF(4.0, 12.0));
      return;
    case Glyph::Home:
      painter.drawPolyline(
          QPolygonF({QPointF(2.5, 8.0), QPointF(8.0, 3.5), QPointF(13.5, 8.0)}));
      painter.drawPolyline(QPolygonF(
          {QPointF(4.0, 7.2), QPointF(4.0, 13.0), QPointF(12.0, 13.0), QPointF(12.0, 7.2)}));
      painter.drawRect(QRectF(6.5, 9.0, 3.0, 4.0));
      return;
    case Glyph::Plus:
      painter.drawLine(QPointF(8.0, 3.0), QPointF(8.0, 13.0));
      painter.drawLine(QPointF(3.0, 8.0), QPointF(13.0, 8.0));
      return;
    case Glyph::Search:
      painter.drawEllipse(QPointF(7.0, 7.0), 4.0, 4.0);
      painter.drawLine(QPointF(10.0, 10.0), QPointF(13.5, 13.5));
      return;
    case Glyph::Filter:
      painter.drawLine(QPointF(3.0, 4.5), QPointF(13.0, 4.5));
      painter.drawEllipse(QPointF(6.0, 4.5), 1.3, 1.3);
      painter.drawLine(QPointF(3.0, 8.0), QPointF(13.0, 8.0));
      painter.drawEllipse(QPointF(10.0, 8.0), 1.3, 1.3);
      painter.drawLine(QPointF(3.0, 11.5), QPointF(13.0, 11.5));
      painter.drawEllipse(QPointF(7.5, 11.5), 1.3, 1.3);
      return;
    case Glyph::SortArrows:
      painter.drawLine(QPointF(5.3, 3.3), QPointF(5.3, 12.7));
      painter.drawPolyline(
          QPolygonF({QPointF(3.3, 5.3), QPointF(5.3, 3.3), QPointF(7.3, 5.3)}));
      painter.drawLine(QPointF(10.7, 3.3), QPointF(10.7, 12.7));
      painter.drawPolyline(
          QPolygonF({QPointF(8.7, 10.7), QPointF(10.7, 12.7), QPointF(12.7, 10.7)}));
      return;
    case Glyph::ChevronDown:
      painter.drawPolyline(QPolygonF({QPointF(4.0, 6.0), QPointF(8.0, 10.0), QPointF(12.0, 6.0)}));
      return;
    case Glyph::Play:
      painter.setPen(Qt::NoPen);
      painter.setBrush(color);
      painter.drawPolygon(
          QPolygonF({QPointF(5.5, 4.3), QPointF(5.5, 11.7), QPointF(12.2, 8.0)}));
      return;
    case Glyph::CheckCircle:
      painter.drawEllipse(QPointF(8.0, 8.0), 5.5, 5.5);
      painter.drawPolyline(
          QPolygonF({QPointF(5.7, 8.3), QPointF(7.3, 10.0), QPointF(10.7, 6.2)}));
      return;
    case Glyph::Download:
      painter.drawLine(QPointF(8.0, 3.0), QPointF(8.0, 10.3));
      painter.drawPolyline(
          QPolygonF({QPointF(5.2, 8.5), QPointF(8.0, 11.3), QPointF(10.8, 8.5)}));
      painter.drawLine(QPointF(4.0, 13.0), QPointF(12.0, 13.0));
      return;
    case Glyph::Sliders:
      for (const qreal y : {4.5, 8.0, 11.5}) painter.drawLine(QPointF(2.5, y), QPointF(13.5, y));
      painter.setBrush(color);
      painter.drawEllipse(QPointF(10.5, 4.5), 1.6, 1.6);
      painter.drawEllipse(QPointF(5.5, 8.0), 1.6, 1.6);
      painter.drawEllipse(QPointF(9.0, 11.5), 1.6, 1.6);
      return;
    case Glyph::Dot:
      painter.setPen(Qt::NoPen);
      painter.setBrush(color);
      painter.drawEllipse(QPointF(8.0, 8.0), 3.5, 3.5);
      return;
    case Glyph::Pin: {
      // A pushpin: head, flared collar, then the needle.
      QPainterPath head;
      head.moveTo(5.5, 2.0);
      head.lineTo(10.5, 2.0);
      head.lineTo(10.0, 3.2);
      head.lineTo(10.0, 6.5);
      head.lineTo(12.5, 9.5);
      head.lineTo(3.5, 9.5);
      head.lineTo(6.0, 6.5);
      head.lineTo(6.0, 3.2);
      head.closeSubpath();
      painter.setBrush(color);
      painter.drawPath(head);
      painter.drawLine(QPointF(8.0, 9.5), QPointF(8.0, 14.5));
      return;
    }
    case Glyph::Store:
      painter.drawRoundedRect(QRectF(3.0, 5.5, 10.0, 8.5), 1.2, 1.2);
      painter.drawArc(QRectF(5.5, 2.5, 5.0, 6.0), 0, 180 * 16);
      return;
    case Glyph::Clock:
      painter.drawEllipse(QPointF(8.0, 8.0), 5.5, 5.5);
      painter.drawLine(QPointF(8.0, 5.0), QPointF(8.0, 8.0));
      painter.drawLine(QPointF(8.0, 8.0), QPointF(10.3, 9.3));
      return;
    case Glyph::Warning:
      painter.drawPolygon(
          QPolygonF({QPointF(8.0, 3.3), QPointF(14.0, 13.5), QPointF(2.0, 13.5)}));
      painter.drawLine(QPointF(8.0, 7.2), QPointF(8.0, 10.2));
      painter.setPen(Qt::NoPen);
      painter.setBrush(color);
      painter.drawEllipse(QPointF(8.0, 11.8), 0.7, 0.7);
      return;
    case Glyph::CircleX:
      painter.drawEllipse(QPointF(8.0, 8.0), 5.5, 5.5);
      painter.drawLine(QPointF(6.2, 6.2), QPointF(9.8, 9.8));
      painter.drawLine(QPointF(9.8, 6.2), QPointF(6.2, 9.8));
      return;
    case Glyph::Moon: {
      QPainterPath outer;
      outer.addEllipse(QPointF(7.5, 8.0), 5.2, 5.2);
      QPainterPath inner;
      inner.addEllipse(QPointF(10.0, 6.0), 4.6, 4.6);
      painter.setPen(Qt::NoPen);
      painter.setBrush(color);
      painter.drawPath(outer.subtracted(inner));
      return;
    }
    case Glyph::EyeSlash:
      painter.drawEllipse(QRectF(3.0, 5.5, 10.0, 5.0));
      painter.drawEllipse(QPointF(8.0, 8.0), 1.3, 1.3);
      painter.drawLine(QPointF(3.3, 3.3), QPointF(12.7, 12.7));
      return;
    case Glyph::Wrench: {
      QPainterPath ring;
      ring.arcMoveTo(QRectF(8.3, 2.3, 5.4, 5.4), 200.0);
      ring.arcTo(QRectF(8.3, 2.3, 5.4, 5.4), 200.0, 250.0);
      painter.drawPath(ring);
      painter.drawLine(QPointF(9.6, 7.4), QPointF(3.0, 14.0));
      return;
    }
    case Glyph::Image:
      painter.drawRoundedRect(QRectF(2.5, 3.5, 11.0, 9.0), 1.0, 1.0);
      painter.drawEllipse(QPointF(6.0, 6.7), 1.0, 1.0);
      painter.drawPolyline(QPolygonF({QPointF(3.0, 11.0), QPointF(6.5, 7.5), QPointF(9.0, 10.0),
                                      QPointF(11.0, 8.0), QPointF(13.0, 10.0)}));
      return;
    case Glyph::Trash:
      painter.drawLine(QPointF(3.5, 5.0), QPointF(12.5, 5.0));
      painter.drawPolyline(QPolygonF({QPointF(6.3, 5.0), QPointF(6.3, 3.7), QPointF(9.7, 3.7),
                                      QPointF(9.7, 5.0)}));
      painter.drawRoundedRect(QRectF(4.7, 5.0, 6.6, 8.0), 1.0, 1.0);
      return;
    case Glyph::Refresh: {
      const QRectF bounds(2.7, 2.7, 10.6, 10.6);
      QPainterPath top;
      top.arcMoveTo(bounds, 55.0);
      top.arcTo(bounds, 55.0, 200.0);
      painter.drawPath(top);
      painter.drawPolyline(
          QPolygonF({QPointF(11.6, 2.9), QPointF(13.3, 5.3), QPointF(10.6, 5.9)}));
      QPainterPath bottom;
      bottom.arcMoveTo(bounds, 235.0);
      bottom.arcTo(bounds, 235.0, 200.0);
      painter.drawPath(bottom);
      painter.drawPolyline(
          QPolygonF({QPointF(4.4, 13.1), QPointF(2.7, 10.7), QPointF(5.4, 10.1)}));
      return;
    }
    case Glyph::Keyboard:
      painter.drawRoundedRect(QRectF(2.0, 5.0, 12.0, 8.0), 1.0, 1.0);
      painter.setPen(Qt::NoPen);
      painter.setBrush(color);
      for (const qreal x : {4.0, 6.5, 9.0, 11.5}) painter.drawRect(QRectF(x, 7.0, 1.2, 1.2));
      painter.drawRoundedRect(QRectF(4.0, 9.7, 8.7, 1.4), 0.6, 0.6);
      return;
    case Glyph::Info:
      painter.drawEllipse(QPointF(8.0, 8.0), 5.5, 5.5);
      painter.drawLine(QPointF(8.0, 7.3), QPointF(8.0, 11.0));
      painter.setPen(Qt::NoPen);
      painter.setBrush(color);
      painter.drawEllipse(QPointF(8.0, 5.2), 0.75, 0.75);
      return;
    case Glyph::Monitor:
      painter.drawRoundedRect(QRectF(2.0, 3.0, 12.0, 8.0), 1.2, 1.2);
      painter.drawLine(QPointF(8.0, 11.0), QPointF(8.0, 13.5));
      painter.drawLine(QPointF(5.5, 13.5), QPointF(10.5, 13.5));
      return;
    case Glyph::Sidebar:
      painter.drawRoundedRect(QRectF(2.0, 3.0, 12.0, 10.0), 1.2, 1.2);
      painter.drawLine(QPointF(6.0, 3.0), QPointF(6.0, 13.0));
      return;
    case Glyph::Folder:
      painter.drawPolygon(QPolygonF({QPointF(2.0, 4.0), QPointF(6.0, 4.0), QPointF(7.5, 5.5), QPointF(14.0, 5.5),
                                     QPointF(14.0, 12.5), QPointF(2.0, 12.5)}));
      return;
    case Glyph::Layers:
      painter.drawPolygon(QPolygonF({QPointF(8.0, 2.5), QPointF(14.0, 5.8), QPointF(8.0, 9.1), QPointF(2.0, 5.8)}));
      painter.drawPolyline(QPolygonF({QPointF(2.0, 9.0), QPointF(8.0, 12.3), QPointF(14.0, 9.0)}));
      return;
    case Glyph::Target:
      painter.drawEllipse(QPointF(8.0, 8.0), 5.5, 5.5);
      painter.drawEllipse(QPointF(8.0, 8.0), 2.0, 2.0);
      return;
    case Glyph::Grid:
      for (const qreal x : {2.5, 9.0}) {
        for (const qreal y : {2.5, 9.0}) painter.drawRoundedRect(QRectF(x, y, 4.5, 4.5), 0.8, 0.8);
      }
      return;
    case Glyph::ArrowLeft:
      painter.drawLine(QPointF(3.0, 8.0), QPointF(13.0, 8.0));
      painter.drawPolyline(QPolygonF({QPointF(7.0, 4.0), QPointF(3.0, 8.0), QPointF(7.0, 12.0)}));
      return;
    case Glyph::Undo: {
      // A counter-clockwise arc with its arrowhead at the start.
      QPainterPath arc;
      arc.arcMoveTo(QRectF(3.0, 3.0, 10.0, 10.0), 160.0);
      arc.arcTo(QRectF(3.0, 3.0, 10.0, 10.0), 160.0, 250.0);
      painter.drawPath(arc);
      painter.drawPolyline(QPolygonF({QPointF(2.6, 3.8), QPointF(3.2, 6.8), QPointF(6.2, 6.0)}));
      return;
    }
    case Glyph::Eye:
      painter.drawEllipse(QRectF(2.5, 4.5, 11.0, 7.0));
      painter.drawEllipse(QPointF(8.0, 8.0), 1.6, 1.6);
      return;
    case Glyph::External:
      painter.drawPolyline(QPolygonF({QPointF(7.0, 3.5), QPointF(3.5, 3.5), QPointF(3.5, 12.5), QPointF(12.5, 12.5),
                                      QPointF(12.5, 9.0)}));
      painter.drawLine(QPointF(8.0, 8.0), QPointF(13.0, 3.0));
      painter.drawPolyline(QPolygonF({QPointF(9.5, 3.0), QPointF(13.0, 3.0), QPointF(13.0, 6.5)}));
      return;
    case Glyph::Grip:
    case Glyph::More: {
      painter.setPen(Qt::NoPen);
      painter.setBrush(color);
      const bool grip = glyph == Glyph::Grip;
      for (const qreal along : {4.0, 8.0, 12.0}) {
        if (grip) {
          painter.drawEllipse(QPointF(6.0, along), 1.0, 1.0);
          painter.drawEllipse(QPointF(10.0, along), 1.0, 1.0);
        } else {
          painter.drawEllipse(QPointF(along, 8.0), 1.2, 1.2);
        }
      }
      return;
    }
  }
}

QPixmap Render(Glyph glyph, int size, const QColor& color) {
  QPixmap pixmap(size, size);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.scale(size / kGrid, size / kGrid);
  PaintGlyph(painter, glyph, color);
  return pixmap;
}

}  // namespace

QIcon For(Glyph glyph) { return For(glyph, theme::Current().text); }

QIcon For(Glyph glyph, const QColor& color) {
  QIcon icon;
  for (const int size : {16, 20, 24, 32, 48}) icon.addPixmap(Render(glyph, size, color));
  return icon;
}

}  // namespace mira_gui::icons
