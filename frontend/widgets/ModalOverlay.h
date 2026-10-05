#pragma once

#include <QColor>
#include <QRect>
#include <QWidget>

#include <functional>

namespace mira_gui {

// A dimmed backdrop for a card laid over the window. A click that lands here
// (never on the card itself, which is a child widget and consumes its own
// clicks first) closes the card, same as clicking outside any other modal.
class ModalOverlay : public QWidget {
public:
  explicit ModalOverlay(QWidget* parent);

  std::function<void()> on_backdrop_clicked;
  // Set: painted here instead of by a stylesheet, around `clear` (in this
  // widget's coordinates), which stays undimmed so its live changes show.
  QColor scrim;
  std::function<QRect()> clear;

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
};

}  // namespace mira_gui
