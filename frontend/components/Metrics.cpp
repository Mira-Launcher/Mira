#include "Metrics.h"

#include <algorithm>

#include "../theme/Theme.h"

namespace mira_gui {

int CardColumnWidth() {
  return std::max(kCardColumn, theme::TextPx(kCardColumn));
}

int ColumnsWidth(int columns, int column_width) {
  return columns * column_width + (columns - 1) * kColumnGap;
}

int SettingsColumnWidth(int room) {
  return std::min(room, room >= kWideRoom ? kWideColumn : kColumn);
}

}  // namespace mira_gui
