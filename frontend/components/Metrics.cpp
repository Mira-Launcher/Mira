#include "Metrics.h"

#include <algorithm>

namespace mira_gui {

int SettingsColumnWidth(int room) {
  return std::min(room, room >= kWideRoom ? kWideColumn : kColumn);
}

}  // namespace mira_gui
