#pragma once

#include <Qt>

namespace mira_gui {

// Enter, Return or Space: the keys that press a focused tile or label the way a click does.
inline bool IsActivationKey(int key) {
  return key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space;
}

}  // namespace mira_gui
