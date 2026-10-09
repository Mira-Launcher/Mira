#pragma once

#include <QHash>
#include <QTimer>

#include <vector>

#include "Page.h"

namespace mira_gui::bigscreen {

// Rows of covers (Continue playing, Pinned, each tag, each source, Installed)
// under the focused game's name and status. View cycles a row's order. The focused row stays put and
// rows above it slide out of view.
class HomePage : public Page {
  Q_OBJECT

public:
  explicit HomePage(BigScreenWindow* window);

  bool Navigate(Nav nav) override;
  QList<Hint> Hints() const override;
  void Shown() override;

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
  QTimer animation_;
  QTimer rebuild_;
};

}  // namespace mira_gui::bigscreen
