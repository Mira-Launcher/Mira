#pragma once

#include <QFrame>

namespace mira_gui {

// One row of a list in a popover (downloads, updates): a line over it unless it's the first, or
// the first under a group heading.
class ListRow : public QFrame {
  Q_OBJECT

 public:
  explicit ListRow(bool first, QWidget* parent = nullptr);
};

}  // namespace mira_gui
