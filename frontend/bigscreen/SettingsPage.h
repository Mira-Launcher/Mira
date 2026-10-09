#pragma once

#include <QElapsedTimer>
#include <QTimer>

#include <functional>
#include <vector>

#include "Page.h"

namespace mira_gui::bigscreen {

// Settings for the couch, in sections: the controller (a page of its own), the
// screen and sound, apps, and the system (power, updates, leaving big screen).
class SettingsPage : public Page {
  Q_OBJECT

public:
  explicit SettingsPage(BigScreenWindow* window);

  bool Navigate(Nav nav) override;
  QList<Hint> Hints() const override;
  void Shown() override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  struct Row {
    QString section;
    QString label;
    QString help;
    // A setting's value, and how Left/Right (-1/+1) change it; empty for an action.
    std::function<QString()> value;
    std::function<void(int)> change;
    // What A does on an action row.
    std::function<void()> act;
  };

  void Build();
  void PaintTest(QPainter& painter, double u);

  void OpenController();
  void CloseController();

  // The rows shown: the main list, or the controller's page.
  std::vector<Row> rows_;
  std::vector<Row> main_rows_;
  std::vector<Row> controller_rows_;
  bool in_controller_ = false;
  int focus_ = 0;
  // Where the main list was, to return to.
  int main_focus_ = 0;
  // The button test: every button lit while it's held.
  bool testing_ = false;
  QTimer test_refresh_;
  QElapsedTimer test_back_;
};

}  // namespace mira_gui::bigscreen
