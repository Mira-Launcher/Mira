#include "NavRow.h"

namespace mira_gui {

NavRow::NavRow(const QString& text, QWidget* parent) : QPushButton(text, parent) {
  setFlat(true);
}

}  // namespace mira_gui
