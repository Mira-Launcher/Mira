#pragma once

#include <QColor>
#include <QRect>
#include <QWidget>
#include <functional>

namespace mira_gui {

// A backdrop for a card laid over the window, dimmed by the theme's modal_scrim_alpha. A click that
// lands here (never on the card itself, which is a child widget and consumes its own clicks first)
// closes the card, same as clicking outside any other modal.
class ModalOverlay : public QWidget {
 public:
  explicit ModalOverlay(QWidget* parent);

  std::function<void()> on_backdrop_clicked;
  // A part left undimmed (in this widget's coordinates), so its live changes show.
  std::function<QRect()> clear;

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
};

// What a modal dims the window with: plain black, since the theme's dark surfaces already sit close
// to black and tinting toward them barely dims anything.
QColor ModalScrim();

}  // namespace mira_gui
