#pragma once

#include <QWidget>

class QSlider;
class QToolButton;

namespace mira_gui {

// The custom title bar in place of a native one: brand, tile size, Activity,
// Refresh, Shortcuts, About, then the window's own minimize/maximize/close.
// Its background (and the labels on it, which pass clicks up) moves the
// window; a double click maximizes it.
class TopBar : public QWidget {
  Q_OBJECT

 public:
  TopBar(int min_tile, int max_tile, int tile, QWidget* parent);

  // The tile size slider; the window points it at whichever page shows tiles.
  QSlider* zoom() const { return zoom_; }
  QToolButton* activity_button() const { return activity_; }
  // Running downloads and installs, shown beside the Activity icon.
  void SetActivityCount(int running);
  // The window's maximized state changed: the button's icon and tooltip follow.
  void SyncMaximized();

 signals:
  void ActivityClicked();
  void RefreshClicked();
  void ShortcutsClicked();
  void AboutClicked();

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;

 private:
  // Redrawn rather than stored: each glyph is painted in the theme's text
  // color, so a theme change has to regenerate them.
  void ApplyIcons();
  void ToggleMaximize();

  QSlider* zoom_ = nullptr;
  QToolButton* activity_ = nullptr;
  QToolButton* refresh_ = nullptr;
  QToolButton* shortcuts_ = nullptr;
  QToolButton* about_ = nullptr;
  QToolButton* minimize_ = nullptr;
  QToolButton* maximize_ = nullptr;
  QToolButton* close_ = nullptr;
};

}  // namespace mira_gui
