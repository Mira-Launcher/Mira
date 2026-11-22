#pragma once

#include <QAbstractButton>

class QVariantAnimation;

namespace mira_gui {

// An on/off control drawn as a sliding switch, the settings rows' toggle.
class Switch : public QAbstractButton {
  Q_OBJECT

 public:
  explicit Switch(QWidget* parent = nullptr);
  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  qreal position_ = 0;  // 0 is off, 1 is on; animated between the two
  QVariantAnimation* slide_ = nullptr;
};

}  // namespace mira_gui
