#pragma once

#include "Page.h"

namespace mira_gui::bigscreen {

// One game: its cover, status and play time, with Play, Install, Pause,
// Pin and Uninstall as its state allows.
class GamePage : public Page {
  Q_OBJECT

public:
  explicit GamePage(BigScreenWindow* window);

  // `from` is where Back returns to.
  void Open(const Item& item, Page* from);
  Page* from() const { return from_; }

  bool Navigate(Nav nav) override;
  QList<Hint> Hints() const override;
  void Shown() override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  enum class Action { Play, Stop, Install, Pause, Resume, Cancel, Pin, Uninstall };
  struct Button {
    Action action;
    QString label;
  };
  std::vector<Button> Buttons() const;
  void Refresh();

  Item item_;
  Page* from_ = nullptr;
  int focus_ = 0;
};

}  // namespace mira_gui::bigscreen
