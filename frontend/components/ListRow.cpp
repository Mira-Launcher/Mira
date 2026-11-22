#include "ListRow.h"

namespace mira_gui {

ListRow::ListRow(bool first, QWidget* parent) : QFrame(parent) {
  setProperty("first", first);
}

}  // namespace mira_gui
