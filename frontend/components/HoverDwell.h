#pragma once

#include <QModelIndex>
#include <QPersistentModelIndex>
#include <QTimer>
#include <functional>

namespace mira_gui {

// How long the cursor rests on something before its hover card shows: a card popping up on
// everything the cursor merely crosses would be worse than none.
inline constexpr int kHoverDwellMs = 280;

// Calls on_hover with an index once the cursor has rested on it, and with an
// invalid one at once when it moves to another index or off the grid. Held
// as a persistent index, so a row that moves or goes away can't dangle.
class HoverDwell {
 public:
  explicit HoverDwell(std::function<void(const QModelIndex&)> on_hover);
  void Track(const QModelIndex& hovered);

 private:
  std::function<void(const QModelIndex&)> on_hover_;
  QTimer timer_;
  QPersistentModelIndex last_;
};

}  // namespace mira_gui
