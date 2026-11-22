#pragma once

#include "../components/WindowChrome.h"

class QLabel;
class QSlider;
class QToolButton;

namespace mira_gui {

// The library window's title bar: brand, tile size, Activity, Refresh, Shortcuts, About, then the
// window's own buttons.
class TopBar : public TitleBar {
  Q_OBJECT

 public:
  TopBar(int min_tile, int max_tile, int tile, QWidget* parent);

  // The tile size slider; the window points it at whichever page shows tiles.
  QSlider* zoom() const { return zoom_; }
  QToolButton* activity_button() const { return activity_; }
  // Running downloads and installs, shown beside the Activity icon.
  void SetActivityCount(int running);
  QToolButton* updates_button() const { return updates_; }
  // What has a newer release, shown beside the Updates icon.
  void SetUpdatesCount(int pending);
  // The one notice the bar has room for, the most important at the moment; empty text hides it.
  // `busy` keeps it quiet, for what's only reporting progress.
  void SetNotice(const QString& text, bool busy, const QString& tooltip, bool lock = false);
  QWidget* notice_button() const;
  // Hidden mode's note in the middle of the bar, naming `shortcut` as the way out; none for empty.
  void SetHiddenShortcut(const QString& shortcut);

 signals:
  void ActivityClicked();
  void UpdatesClicked();
  void NoticeClicked();
  void BigScreenClicked();
  void RefreshClicked();
  void ShortcutsClicked();
  void AboutClicked();

 protected:
  void resizeEvent(QResizeEvent* event) override;

 private:
  // Centered on the bar, and left out where it would run into the brand or the tools.
  void PlaceHiddenNote();

  QLabel* brand_ = nullptr;
  QLabel* hidden_note_ = nullptr;
  QSlider* zoom_ = nullptr;
  QToolButton* activity_ = nullptr;
  QToolButton* updates_ = nullptr;
  QToolButton* notice_ = nullptr;
  QToolButton* big_screen_ = nullptr;
  QToolButton* refresh_ = nullptr;
  QToolButton* shortcuts_ = nullptr;
  QToolButton* about_ = nullptr;
};

}  // namespace mira_gui
