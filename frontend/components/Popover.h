#pragma once

#include <QPointer>
#include <QWidget>

#include "PopupCard.h"

namespace mira_gui {

// A panel that drops down from a button (Activity, Updates, the filter pill) and closes on a click
// outside it: a Qt::Popup drawn as a card. Subclasses lay out their content in it.
class Popover : public QWidget {
  Q_OBJECT

 public:
  explicit Popover(QWidget* parent = nullptr);

  // Shows it just under `anchor`, lined up with its `align` edge (above it when there's no room
  // below), kept on screen.
  void PopUpBelow(QWidget* anchor, popup::Align align);

  // The space between the anchor and the popover.
  static constexpr int kGap = 4;

 protected:
  // Placed again when its content grows or shrinks while open, so it stays on screen.
  void resizeEvent(QResizeEvent* event) override;

 private:
  void Place();

  QPointer<QWidget> anchor_;
  popup::Align align_ = popup::Align::Start;
};

}  // namespace mira_gui
