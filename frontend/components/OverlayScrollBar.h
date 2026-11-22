#pragma once

#include <QPointer>
#include <QScrollBar>
#include <QWidget>

class QAbstractScrollArea;
class QTimer;
class QVariantAnimation;

namespace mira_gui {

// A scroll bar drawn over its area's content as a thin thumb, taking no room from it. It shows
// while the area scrolls and for a moment after, and while the pointer is on it; the thumb drags.
// The pointer coming into the area doesn't show it: a page opening under the pointer would fade it
// in each time. The area's own bar keeps the position (base.qss sizes it
// to 1 px, undrawn), so code that reads or sets the bar works as before. A bar the area turns
// off never shows.
class OverlayScrollBar : public QWidget {
  Q_OBJECT

 public:
  OverlayScrollBar(QAbstractScrollArea* area, QScrollBar* bar);

  QScrollBar* Bar() const { return bar_; }
  // Whether the thumb shows (or is fading); faded out, it lets the pointer through.
  bool Shown() const;
  // The thumb's painted rect within this widget, which is as big as its grabbable area; empty when
  // there's nothing to scroll.
  QRectF Thumb() const;

 protected:
  void paintEvent(QPaintEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  bool Vertical() const;
  bool Scrollable() const;
  QRect Strip() const;        // where the thumb travels, in the area's coordinates
  QRect ThumbInArea() const;  // the grabbable thumb, in the area's coordinates
  void Place();
  void Reveal();
  void FadeOut();

  QPointer<QAbstractScrollArea> area_;
  QPointer<QScrollBar> bar_;
  QTimer* linger_;
  QVariantAnimation* fade_;
  qreal opacity_ = 0;
  bool hovered_ = false;
  qreal drag_offset_ = -1;  // where on the thumb the press was, while dragging
};

// Gives every scroll area in the app overlay bars, as they're first polished.
void SetUpOverlayScrollBars();

}  // namespace mira_gui
