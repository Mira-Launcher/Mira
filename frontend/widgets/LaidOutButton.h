#pragma once

#include <QLayout>
#include <QPushButton>

namespace mira_gui {

// A button sized by the layout inside it. QPushButton's own size hint only counts its text, so
// wrapped labels inside got squeezed whenever the fonts were bigger than when it was made.
class LaidOutButton : public QPushButton {
 public:
  using QPushButton::QPushButton;
  QSize sizeHint() const override {
    return layout() != nullptr ? layout()->sizeHint() : QPushButton::sizeHint();
  }
  QSize minimumSizeHint() const override {
    return layout() != nullptr ? layout()->minimumSize() : QPushButton::minimumSizeHint();
  }
  bool hasHeightForWidth() const override {
    return layout() != nullptr && layout()->hasHeightForWidth();
  }
  int heightForWidth(int width) const override {
    return layout() != nullptr ? layout()->totalHeightForWidth(width)
                               : QPushButton::heightForWidth(width);
  }
};

}  // namespace mira_gui
