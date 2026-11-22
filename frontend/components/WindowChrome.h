#pragma once

#include <QWidget>

class QToolButton;

namespace mira_gui {

// A frameless window's own title bar: a press on the bar or a label on it moves the window, a
// double click maximizes or restores it. A control on it takes its own clicks.
class TitleBar : public QWidget {
  Q_OBJECT

 public:
  explicit TitleBar(QWidget* parent = nullptr);

 protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
};

// Minimize, Maximize (Restore while maximized) and Close for the window it's in.
class WindowButtons : public QWidget {
  Q_OBJECT

 public:
  explicit WindowButtons(QWidget* parent = nullptr);

 protected:
  void showEvent(QShowEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  void Sync();

  QToolButton* maximize_;
  QWidget* watched_ = nullptr;
};

void ToggleMaximized(QWidget* window);

}  // namespace mira_gui
