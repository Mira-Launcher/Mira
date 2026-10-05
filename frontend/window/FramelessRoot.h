#pragma once

#include <QObject>
#include <QWidget>

class QMainWindow;

namespace mira_gui {

// The strip around a frameless window's content that resizes it.
inline constexpr int kResizeMargin = 5;

// The frameless window's own background: a thin margin around the real
// content, the only thing left to grab for an edge resize with no OS
// titlebar. QWindow::startSystemResize hands the drag to the compositor,
// which is what makes this work under Wayland.
class FramelessRoot : public QWidget {
public:
  explicit FramelessRoot(QMainWindow* window);

protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;

private:
  // Edges under `pos` that resize. The top edge only resizes at its
  // corners; the rest of it moves the window, since a drag up there (often
  // toward the top of the screen, e.g. out of a tiled corner) is meant to
  // move it. Wayland doesn't tell a window where it is, so this can't
  // depend on the screen edges.
  Qt::Edges ResizableEdgesAt(const QPoint& pos) const;

  QMainWindow* window_;
};

// A click on anything that can't take focus itself (a page's background, a
// label) drops keyboard focus from a text box or button, the way a browser
// does, so Enter and Delete go back to the window's own shortcuts.
class FocusDropper : public QObject {
public:
  explicit FocusDropper(QWidget* window) : QObject(window), window_(window) {}

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  QWidget* window_;
};

}  // namespace mira_gui
