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

}  // namespace mira_gui
