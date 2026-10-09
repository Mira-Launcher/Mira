#include "Notice.h"

#include <QPainter>
#include <QScreen>

#include "../theme/Theme.h"
#include "Paint.h"

namespace mira_gui::bigscreen {

Notice::Notice(QWidget* screen_of)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                           Qt::WindowDoesNotAcceptFocus | Qt::X11BypassWindowManagerHint |
                           Qt::WindowTransparentForInput),
      screen_of_(screen_of) {
  setAttribute(Qt::WA_ShowWithoutActivating);
  setAttribute(Qt::WA_TranslucentBackground);
  hide_.setSingleShot(true);
  hide_.setInterval(4500);
  connect(&hide_, &QTimer::timeout, this, &QWidget::hide);
}

void Notice::Show(const QString& title, const QString& detail) {
  title_ = title;
  detail_ = detail;
  const QRect screen = screen_of_->screen()->geometry();
  const double u = screen.height() / 49.0;
  const QSize size(qRound(u * 22), qRound(u * (detail.isEmpty() ? 3.6 : 5.4)));
  setGeometry(screen.right() - size.width() - qRound(u * 1.5), screen.top() + qRound(u * 1.5), size.width(),
              size.height());
  show();
  raise();
  update();
  hide_.start();
}

void Notice::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const theme::Tokens& tokens = theme::Current();
  const double u = screen_of_->screen()->geometry().height() / 49.0;
  QColor back = tokens.surface;
  back.setAlpha(240);
  painter.setPen(QPen(Accent(), u * 0.1));
  painter.setBrush(back);
  painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), u * 0.6, u * 0.6);
  const QRectF inner = QRectF(rect()).adjusted(u * 1.2, u * 0.9, -u * 1.2, -u * 0.9);
  painter.setPen(tokens.text);
  painter.setFont(Font(u, 1.05, QFont::Bold));
  painter.drawText(inner, Qt::AlignTop | Qt::AlignLeft, painter.fontMetrics().elidedText(title_, Qt::ElideRight, int(inner.width())));
  if (detail_.isEmpty()) return;
  painter.setPen(tokens.text_muted);
  painter.setFont(Font(u, 0.9));
  painter.drawText(inner.adjusted(0, u * 1.9, 0, 0), Qt::AlignTop | Qt::AlignLeft,
                   painter.fontMetrics().elidedText(detail_, Qt::ElideRight, int(inner.width())));
}

}  // namespace mira_gui::bigscreen
