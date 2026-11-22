#include "PopupCard.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QWidget>
#include <algorithm>

#include "../theme/Theme.h"

namespace mira_gui::popup {

void Paint(QWidget* widget) {
  const theme::Tokens& tokens = theme::Current();
  QPainter painter(widget);
  painter.setRenderHint(QPainter::Antialiasing);
  QPainterPath path;
  path.addRoundedRect(QRectF(widget->rect()).adjusted(0.5, 0.5, -0.5, -0.5), tokens.radius_panel,
                      tokens.radius_panel);
  painter.fillPath(path, tokens.surface);
  painter.setPen(QPen(tokens.border, 1));
  painter.drawPath(path);
}

QPoint Place(const QRect& anchor, QSize size, bool beside) {
  QScreen* screen = QGuiApplication::screenAt(anchor.center());
  if (screen == nullptr) screen = QGuiApplication::primaryScreen();
  return Place(anchor, size, beside,
               screen != nullptr ? screen->availableGeometry() : QRect(anchor.topLeft(), size));
}

QPoint Place(const QRect& anchor, QSize size, bool beside, const QRect& area) {
  QPoint pos;
  if (beside) {
    pos = QPoint(anchor.right() + 1 + kGap, anchor.top());
    if (pos.x() + size.width() > area.right()) pos.setX(anchor.left() - kGap - size.width());
  } else {
    pos = QPoint(anchor.center().x() - size.width() / 2, anchor.bottom() + 1 + kGap);
    if (pos.y() + size.height() > area.bottom()) pos.setY(anchor.top() - kGap - size.height());
  }
  pos.setX(
      std::clamp(pos.x(), area.left(), std::max(area.left(), area.right() + 1 - size.width())));
  pos.setY(
      std::clamp(pos.y(), area.top(), std::max(area.top(), area.bottom() + 1 - size.height())));
  return pos;
}

}  // namespace mira_gui::popup
