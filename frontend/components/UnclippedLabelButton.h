#pragma once

#include <QPushButton>

namespace mira_gui {

// A button with a centered label that may run into its padding, so a checked segment or tab's
// semibold text fits where the regular text sized it. Qt's stylesheet style cuts a label at the
// padding, which in a font with a much wider bold (DejaVu) cut both ends off.
class UnclippedLabelButton : public QPushButton {
 public:
  using QPushButton::QPushButton;

 protected:
  void paintEvent(QPaintEvent* event) override;
};

}  // namespace mira_gui
