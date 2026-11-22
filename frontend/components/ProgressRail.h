#pragma once

#include <QColor>
#include <QWidget>

class QPainter;
class QRectF;
class QTimer;

namespace mira_gui {

// The progress rail (paint::Rail) as a widget, for rows (downloads, runners). Animates itself
// while busy.
class ProgressRail : public QWidget {
  Q_OBJECT

 public:
  explicit ProgressRail(QWidget* parent = nullptr);

  // 0..1, or below 0 for busy.
  void SetProgress(double fraction);
  // Quieter, with no groove: for work behind something already shown.
  void SetFaint(bool faint);
  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;
  // Ticks only while on screen.
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

 private:
  double fraction_ = -1;
  bool faint_ = false;
  QTimer* tick_ = nullptr;
};

}  // namespace mira_gui
