#include "ZoomSettle.h"

#include <QTimer>

#include "ArtworkStore.h"

namespace mira_gui {

ZoomSettle::ZoomSettle(QObject* parent, ArtworkStore* artwork, int width,
                       std::function<void()> settled)
    : QObject(parent),
      artwork_(artwork),
      settled_(std::move(settled)),
      timer_(new QTimer(this)),
      width_(width) {
  timer_->setSingleShot(true);
  timer_->setInterval(150);
  connect(timer_, &QTimer::timeout, this, [this] {
    artwork_->SetQuickScaling(false);
    for (const int width : std::as_const(widths_)) {
      if (width != width_) artwork_->ForgetWidth(width);
    }
    widths_.clear();
    settled_();
  });
}

void ZoomSettle::Step(int width) {
  if (width == width_) return;
  widths_.insert(width_);
  width_ = width;
  artwork_->SetQuickScaling(true);
  timer_->start();
}

}  // namespace mira_gui
