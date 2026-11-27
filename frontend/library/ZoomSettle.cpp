#include "ZoomSettle.h"

#include <QTimer>
#include <QWidget>
#include <utility>

#include "ArtworkStore.h"

namespace mira_gui {

ZoomSettle::ZoomSettle(QObject* parent, ArtworkStore* artwork, int width,
                       std::function<void()> settled)
    : QObject(parent),
      artwork_(artwork),
      settled_(std::move(settled)),
      timer_(new QTimer(this)),
      width_(width) {
  artwork_->HoldWidth(width_);
  timer_->setSingleShot(true);
  timer_->setInterval(150);
  connect(timer_, &QTimer::timeout, this, [this] {
    if (std::exchange(quick_, false)) artwork_->SetQuickScaling(false);
    for (const int width : std::as_const(widths_)) {
      if (width != width_) artwork_->ForgetWidth(width);
    }
    widths_.clear();
    settled_();
  });
}

ZoomSettle::~ZoomSettle() {
  if (artwork_ == nullptr) return;
  if (quick_) artwork_->SetQuickScaling(false);
  artwork_->LetGoWidth(width_);
}

void ZoomSettle::Step(int width) {
  if (width == width_) return;
  widths_.insert(width_);
  artwork_->LetGoWidth(width_);
  artwork_->HoldWidth(width);
  width_ = width;
  // Quick scaling is the whole app's: a view off screen (the library grid behind the Tags page as
  // hidden mode swaps its size) only changes width, its old covers going once it settles.
  if (const auto* view = qobject_cast<const QWidget*>(parent());
      view == nullptr || view->isVisible()) {
    if (!std::exchange(quick_, true)) artwork_->SetQuickScaling(true);  // once per zoom
  }
  timer_->start();
}

}  // namespace mira_gui
