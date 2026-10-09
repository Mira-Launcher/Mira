#pragma once

#include <vector>

#include "Page.h"

namespace mira_gui::bigscreen {

// Store installs running or paused, with pause, resume and cancel, then the
// owned games that aren't installed, to install from.
class DownloadsPage : public Page {
  Q_OBJECT

public:
  explicit DownloadsPage(BigScreenWindow* window);

  bool Navigate(Nav nav) override;
  QList<Hint> Hints() const override;
  void Shown() override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  void Refresh();
  // The top of row `i` in units, before scrolling.
  double RowTop(int i) const;

  // Installs first, then `ready` games to install.
  std::vector<Item> items_;
  int installs_ = 0;
  int focus_ = 0;
};

}  // namespace mira_gui::bigscreen
