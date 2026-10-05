#include "FramelessRoot.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QMainWindow>
#include <QMouseEvent>
#include <QWindow>

namespace mira_gui {
namespace {

Qt::Edges EdgesAt(const QSize& size, const QPoint& pos) {
  Qt::Edges edges;
  if (pos.x() <= kResizeMargin) edges |= Qt::LeftEdge;
  if (pos.x() >= size.width() - kResizeMargin) edges |= Qt::RightEdge;
  if (pos.y() <= kResizeMargin) edges |= Qt::TopEdge;
  if (pos.y() >= size.height() - kResizeMargin) edges |= Qt::BottomEdge;
  return edges;
}

}  // namespace

FramelessRoot::FramelessRoot(QMainWindow* window) : window_(window) { setMouseTracking(true); }

void FramelessRoot::mousePressEvent(QMouseEvent* event) {
  QWindow* handle = window_->windowHandle();
  if (event->button() == Qt::LeftButton && !window_->isMaximized() && handle != nullptr) {
    const Qt::Edges edges = ResizableEdgesAt(event->pos());
    if (edges != Qt::Edges()) {
      handle->startSystemResize(edges);
      event->accept();
      return;
    }
    // The strip above the top bar moves the window like the bar does.
    if (event->pos().y() <= kResizeMargin) {
      handle->startSystemMove();
      event->accept();
      return;
    }
  }
  QWidget::mousePressEvent(event);
}

void FramelessRoot::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->pos().y() <= kResizeMargin && ResizableEdgesAt(event->pos()) == Qt::Edges()) {
    window_->isMaximized() ? window_->showNormal() : window_->showMaximized();
    return;
  }
  QWidget::mouseDoubleClickEvent(event);
}

void FramelessRoot::mouseMoveEvent(QMouseEvent* event) {
  if (window_->isMaximized()) {
    unsetCursor();
    return;
  }
  const Qt::Edges edges = ResizableEdgesAt(event->pos());
  if ((edges & Qt::LeftEdge) && (edges & Qt::TopEdge)) {
    setCursor(Qt::SizeFDiagCursor);
  } else if ((edges & Qt::RightEdge) && (edges & Qt::BottomEdge)) {
    setCursor(Qt::SizeFDiagCursor);
  } else if ((edges & Qt::RightEdge) && (edges & Qt::TopEdge)) {
    setCursor(Qt::SizeBDiagCursor);
  } else if ((edges & Qt::LeftEdge) && (edges & Qt::BottomEdge)) {
    setCursor(Qt::SizeBDiagCursor);
  } else if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
    setCursor(Qt::SizeHorCursor);
  } else if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
    setCursor(Qt::SizeVerCursor);
  } else {
    unsetCursor();
  }
}

Qt::Edges FramelessRoot::ResizableEdgesAt(const QPoint& pos) const {
  constexpr int kCorner = 14;
  Qt::Edges edges = EdgesAt(size(), pos);
  if (!(edges & Qt::TopEdge)) return edges;
  if (pos.x() <= kCorner) return Qt::TopEdge | Qt::LeftEdge;
  if (pos.x() >= width() - kCorner) return Qt::TopEdge | Qt::RightEdge;
  return edges & ~Qt::Edges(Qt::TopEdge);
}

bool FocusDropper::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() != QEvent::MouseButtonPress) return false;
  auto* target = qobject_cast<QWidget*>(watched);
  if (target == nullptr || target->window() != window_ || target->focusPolicy() & Qt::ClickFocus) return false;
  // A viewport forwards to its view, which takes focus on its own.
  if (qobject_cast<QAbstractScrollArea*>(target->parentWidget()) != nullptr &&
      target->parentWidget()->focusPolicy() & Qt::ClickFocus) {
    return false;
  }
  if (QWidget* focused = QApplication::focusWidget(); focused != nullptr && focused->window() == window_) {
    focused->clearFocus();
  }
  return false;
}

}  // namespace mira_gui
