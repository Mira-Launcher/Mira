#include "HoverDwell.h"

namespace mira_gui {

HoverDwell::HoverDwell(std::function<void(const QModelIndex&)> on_hover)
    : on_hover_(std::move(on_hover)) {
  timer_.setSingleShot(true);
  QObject::connect(&timer_, &QTimer::timeout, [this] {
    if (last_.isValid()) on_hover_(last_);
  });
}

void HoverDwell::Track(const QModelIndex& hovered) {
  if (QPersistentModelIndex(hovered) == last_) return;
  last_ = hovered;
  timer_.stop();
  on_hover_(QModelIndex());  // hide at once on change or leave
  if (hovered.isValid()) timer_.start(kHoverDwellMs);
}

}  // namespace mira_gui
