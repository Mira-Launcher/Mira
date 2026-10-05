#pragma once

#include <QWidget>

class QLabel;

namespace mira_gui {

// What the right-hand panel shows with nothing selected: what this is, which
// version of it, who wrote it and under what license. A blank panel was the
// alternative, and this is the one place in the window those facts fit.
class AboutPanel : public QWidget {
  Q_OBJECT

public:
  explicit AboutPanel(QWidget* parent = nullptr);

private:
  void ApplyLogo();

  QLabel* logo_ = nullptr;
};

}  // namespace mira_gui
