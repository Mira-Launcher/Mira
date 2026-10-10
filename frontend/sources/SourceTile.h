#pragma once

#include <QFrame>
#include <QPoint>

#include "Sources.h"

class QHBoxLayout;
class QLabel;
class QVBoxLayout;

namespace mira_gui {

// A source as a card in the Sources grids: badge, name and a line under it,
// buttons at the top right, and a body the owner fills. Once clickable, a
// click anywhere but a control emits Clicked.
class SourceTile : public QFrame {
  Q_OBJECT

public:
  SourceTile(const SourceInfo& source, QWidget* parent = nullptr);

  const SourceInfo& Source() const { return source_; }
  QLabel* Name() const { return name_; }
  // The line under the name, elided.
  QLabel* Line() const { return line_; }
  QHBoxLayout* Corner() const { return corner_; }
  QVBoxLayout* Body() const { return body_; }
  void SetClickable(const QString& tooltip);
  // Fades the badge and name, e.g. while the source is off.
  void SetDim(bool dim);

  bool hasHeightForWidth() const override;
  int heightForWidth(int width) const override;

signals:
  void Clicked();

protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;

private:
  SourceInfo source_;
  QLabel* badge_ = nullptr;
  QLabel* name_ = nullptr;
  QLabel* line_ = nullptr;
  QHBoxLayout* corner_ = nullptr;
  QVBoxLayout* body_ = nullptr;
  bool clickable_ = false;
  QPoint pressed_at_;
};

}  // namespace mira_gui
