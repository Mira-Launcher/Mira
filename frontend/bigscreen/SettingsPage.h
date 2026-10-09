#pragma once

#include <QElapsedTimer>
#include <QTimer>

#include <functional>
#include <vector>

#include "Page.h"

namespace mira_gui::bigscreen {

// Settings for the couch: a short list of sub pages (controller, screen and sound,
// apps, system, power) and Exit.
class SettingsPage : public Page {
  Q_OBJECT

public:
  explicit SettingsPage(BigScreenWindow* window);

  bool Navigate(Nav nav) override;
  QList<Hint> Hints() const override;
  void Shown() override;
  void PrefsChanged() override { update(); }

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
    // The sub page A opens, or -1.
    int page = -1;
  };
  struct SubPage {
    QString title;
    std::vector<Row> rows;
  };

  void Build();
  void PaintTest(QPainter& painter, double u);

  void Open(int page);
  void Close();
  const std::vector<Row>& rows() const { return page_ < 0 ? main_rows_ : pages_[size_t(page_)].rows; }

  std::vector<Row> main_rows_;
  std::vector<SubPage> pages_;
  // The sub page shown, or -1 for the main list.
  int page_ = -1;
  int focus_ = 0;
  // Where the main list was, to return to.
  int main_focus_ = 0;
  // The button test: every button lit while it's held.
  bool testing_ = false;
  QTimer test_refresh_;
  QElapsedTimer test_back_;
};

}  // namespace mira_gui::bigscreen
