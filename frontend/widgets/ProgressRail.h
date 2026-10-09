#pragma once

#include <QColor>
#include <QWidget>

class QPainter;
class QRectF;
class QTimer;

namespace mira_gui {

// Mira's one progress look: a thin rounded rail filled with the accent.
// `fraction` is 0..1, or below 0 when nothing reports how far along it is,
// which draws a short segment sliding along, placed by the clock so every
// rail on screen moves together.
void PaintRail(QPainter& painter, const QRectF& track, double fraction, const QColor& fill, const QColor& groove);

// The rail as a widget, for rows (downloads, runners). Animates itself
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

private:
  double fraction_ = -1;
  bool faint_ = false;
  QTimer* tick_ = nullptr;
};

}  // namespace mira_gui
