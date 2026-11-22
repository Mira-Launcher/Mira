#include "ProgressRail.h"

#include <QPainter>
#include <QTimer>

#include "../theme/Theme.h"
#include "paint/Rail.h"

namespace mira_gui {
namespace {

constexpr int kHeight = 5;

}  // namespace

ProgressRail::ProgressRail(QWidget* parent) : QWidget(parent), tick_(new QTimer(this)) {
  setFixedHeight(kHeight);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  tick_->setInterval(33);
  connect(tick_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
  SetProgress(-1);
}

void ProgressRail::SetProgress(double fraction) {
  fraction_ = fraction;
  if (fraction < 0 && isVisible()) {
    tick_->start();
  } else {
    tick_->stop();
  }
  update();
}

void ProgressRail::SetFaint(bool faint) {
  faint_ = faint;
  update();
}

void ProgressRail::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (fraction_ < 0) tick_->start();
}

void ProgressRail::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  tick_->stop();
}

QSize ProgressRail::sizeHint() const {
  return {90, kHeight};
}

void ProgressRail::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  const theme::Tokens& tokens = theme::Current();
  QColor fill = tokens.accent;
  if (faint_) fill.setAlphaF(0.55);
  paint::Rail(painter, QRectF(rect()), fraction_, fill,
              faint_ ? QColor(Qt::transparent) : tokens.border);
}

}  // namespace mira_gui
