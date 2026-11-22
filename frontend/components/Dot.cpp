#include "Dot.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <algorithm>

#include "../theme/Theme.h"

namespace mira_gui {

Dot::Dot(Tone tone, QWidget* parent, QWidget* line_of)
    : QWidget(parent), tone_(tone), line_of_(line_of) {
  setFocusPolicy(Qt::NoFocus);
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  setAttribute(Qt::WA_TransparentForMouseEvents);
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, [this] {
    updateGeometry();
    update();
  });
}

void Dot::SetTone(Tone tone) {
  if (tone == tone_) return;
  tone_ = tone;
  update();
}

int Dot::Diameter() const {
  const QWidget* source = line_of_ != nullptr ? line_of_ : this;
  return std::max(4, source->fontMetrics().height() / 2);
}

QSize Dot::sizeHint() const {
  const QWidget* source = line_of_ != nullptr ? line_of_ : this;
  return {Diameter(), source->fontMetrics().height()};
}

void Dot::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  painter.setBrush(ToneColor(tone_));
  const qreal d = Diameter();
  const qreal line = sizeHint().height();
  painter.drawEllipse(QRectF((width() - d) / 2, (line - d) / 2, d, d));
}

StatusLine::StatusLine(QWidget* parent) : QWidget(parent) {
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(theme::TextPx(6));
  label_ = new QLabel(this);
  label_->setTextFormat(Qt::PlainText);
  // Its own height, never stretched: a taller row would centre the text away from the dot.
  label_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  dot_ = new Dot(Tone::Muted, this, label_);
  layout->addWidget(dot_, 0, Qt::AlignTop);
  layout->addWidget(label_, 1);
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this,
          [layout] { layout->setSpacing(theme::TextPx(6)); });
}

void StatusLine::Set(Tone tone, const QString& text) {
  dot_->SetTone(tone);
  dot_->show();
  label_->setText(text);
}

void StatusLine::SetPlain(const QString& text) {
  dot_->hide();
  label_->setText(text);
}

}  // namespace mira_gui
