#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QTimer>

#include <vector>

#include "Page.h"

namespace mira_gui::bigscreen {

// Rows of covers (Continue playing, Pinned, Media, each tag, each source, Installed, Apps)
// under the focused game's name and status. View cycles a row's order. The focused row stays put and
// rows above it slide out of view.
class HomePage : public Page {
  Q_OBJECT

public:
  explicit HomePage(BigScreenWindow* window);

  bool Navigate(Nav nav) override;
  QList<Hint> Hints() const override;
  void Shown() override;
  void StartBump(int x, int y);
  // Rows ease down while the hero is showcased, and back after.
  void ShowcaseChanged() { animation_.start(); }

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  struct Row {
    QString label;
    std::vector<Item> items;
    int focus = 0;
    double scroll = 0;  // shown, eased toward the focused tile's target
  };

  void Rebuild();
  const Item* Focused() const;
  void FocusChanged();
  double TargetScroll(const Row& row) const;
  void Animate();
  enum class Order { Name, Recent, MostPlayed, kCount };
  Order OrderOf(const Row& row) const;
  void SortRow(Row& row) const;

  // Each row's order, by label, when changed from its own.
  QHash<QString, Order> orders_;

  std::vector<Row> rows_;
  int row_ = 0;
  double row_scroll_ = 0;  // shown, in rows
  double lift_ = 1;  // the focused tile's rise, 0 to 1
  double drop_ = 0;  // how far the rows have moved down for a trailer, 0 to 1
  double info_ = 1;  // the focused game's name and details sliding in, 0 to 1
  QString info_key_;
  // The knock at the end of a row or the list: progress 0 to 1, and its direction.
  double bump_ = 1;
  int bump_x_ = 0, bump_y_ = 0;
  QElapsedTimer frame_clock_;  // since the last animation tick
  QTimer animation_;
  QTimer rebuild_;
};

}  // namespace mira_gui::bigscreen
