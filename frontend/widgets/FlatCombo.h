#pragma once

#include <QComboBox>

namespace mira_gui {

// A dropdown as wide as its chosen value, so in a settings row the value sits by its chevron at
// the row's right edge however long the other options are. The popup still fits the longest.
class FlatCombo : public QComboBox {
  Q_OBJECT

 public:
  explicit FlatCombo(QWidget* parent = nullptr);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;
  void showPopup() override;

 protected:
  void changeEvent(QEvent* event) override;
};

}  // namespace mira_gui
