#pragma once

#include <vector>

#include "Page.h"

namespace mira_gui::bigscreen {

// Store installs running or paused, with pause, resume and cancel.
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

  std::vector<Item> items_;
  int focus_ = 0;
};

}  // namespace mira_gui::bigscreen
