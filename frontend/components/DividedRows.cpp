#include "DividedRows.h"

#include <QPainter>

#include "../theme/Theme.h"

namespace mira_gui {
namespace {

void DrawLine(QPainter& painter, const QWidget* on, int y) {
  painter.drawLine(QPoint(1, y), QPoint(on->width() - 2, y));
}

QColor LineColor() {
  QColor line = theme::Current().border;
  line.setAlpha(150);
  return line;
}

}  // namespace

DividedRows::DividedRows(const QList<QWidget*>* rows, QWidget* parent)
    : QWidget(parent), rows_(rows) {}

void DividedRows::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setPen(LineColor());
  bool first = true;
  for (QWidget* row : *rows_) {
    if (!row->isVisible()) continue;
    if (!first) DrawLine(painter, this, row->y());
    first = false;
  }
}

void DividedFooter::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setPen(LineColor());
  for (QObject* child : children()) {
    auto* row = qobject_cast<QWidget*>(child);
    if (row != nullptr && row->isVisible()) DrawLine(painter, this, row->y());
  }
}

}  // namespace mira_gui
