#include "Badge.h"

#include <QEvent>
#include <QPainter>

#include "../theme/Theme.h"
#include "paint/Pill.h"

namespace mira_gui {
namespace {

constexpr int kPaddingX = 6;  // at the tuned text size, scaled with it

}  // namespace

Badge::Badge(const QString& text, Tone tone, QWidget* parent)
    : QWidget(parent), text_(text), tone_(tone) {
  setFocusPolicy(Qt::NoFocus);
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  setAccessibleName(text);
  // A new theme can change the small text size and every color.
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, [this] {
    updateGeometry();
    update();
  });
}

void Badge::SetText(const QString& text) {
  if (text == text_) return;
  text_ = text;
  setAccessibleName(text);
  updateGeometry();
  update();
}

void Badge::SetTone(Tone tone) {
  tone_ = tone;
  update();
}

void Badge::SetColor(const QColor& color) {
  color_ = color;
  update();
}

QFont Badge::TextFont() const {
  QFont small = font();
  small.setPixelSize(theme::Current().font_size_small);
  return small;
}

QSize Badge::sizeHint() const {
  const QFontMetrics metrics(TextFont());
  return {metrics.horizontalAdvance(text_) + 2 * (theme::TextPx(kPaddingX) + 1),
          metrics.height() + 2};
}

void Badge::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  const QColor color = color_.isValid() ? InkFor(color_) : ToneColor(tone_);
  const QRectF box(QPointF(0, 0), QSizeF(sizeHint()).boundedTo(size()));
  const QRectF at(box.translated(0, (height() - box.height()) / 2));
  paint::Pill(painter, at, paint::PillRadius(at.height(), theme::Current().radius_control),
              QColor(), color);
  painter.setFont(TextFont());
  painter.setPen(color);
  painter.drawText(at, Qt::AlignCenter, text_);
}

void Badge::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::FontChange) updateGeometry();
}

}  // namespace mira_gui
