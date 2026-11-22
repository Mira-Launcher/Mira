#pragma once

#include <QPushButton>

namespace mira_gui {

// A row of a navigation column (the sidebar, Settings' categories): flat, its text at the left,
// the surface on hover and the accent while checked. A row with parts of its own (a count, a cover
// deck) lays them out inside it.
class NavRow : public QPushButton {
  Q_OBJECT

 public:
  explicit NavRow(const QString& text = {}, QWidget* parent = nullptr);
};

}  // namespace mira_gui
