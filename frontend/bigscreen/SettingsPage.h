#pragma once

#include "Page.h"

namespace mira_gui::bigscreen {

// The few settings that matter on the couch, and the way back to the desktop.
class SettingsPage : public Page {
  Q_OBJECT

public:
  explicit SettingsPage(BigScreenWindow* window);

  bool Navigate(Nav nav) override;
  QList<Hint> Hints() const override;
  void Shown() override { update(); }

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  void Change(int step);
  void Act();

  int focus_ = 0;
};

}  // namespace mira_gui::bigscreen
