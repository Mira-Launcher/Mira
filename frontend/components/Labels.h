#pragma once

#include <QColor>
#include <QLabel>
#include <QString>
#include <Qt>

#include "Text.h"

class QWidget;

namespace mira_gui {

// A label in `style` (components/Text). Wraps unless told not to.
QLabel* MakeLabel(QWidget* parent, const QString& text, TextStyle style = text::kBody,
                  bool wrap = true);

// A small caps-style group heading ("PINNED", "SORT") in the muted color.
QLabel* MakeGroupHeading(QWidget* parent, const QString& text);

// A 1 px line in the theme's border color, following theme changes.
// `length` fixes its long side; 0 lets the layout stretch it.
QWidget* MakeDivider(QWidget* parent, Qt::Orientation orientation, int length = 0);

// `widget` keeps its space in the layout while hidden, so showing it doesn't shift its neighbours.
void KeepSpaceWhenHidden(QWidget* widget, bool keep = true);

// One line of text that shrinks with an ellipsis instead of pushing what's
// beside it out of view; the full text is its tooltip while cut.
class ElidedLabel : public QLabel {
  Q_OBJECT

 public:
  explicit ElidedLabel(const QString& text = {}, QWidget* parent = nullptr);
  QSize minimumSizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;
};

// "● " in `color`, for a status line.
QString StatusDot(const QColor& color);

}  // namespace mira_gui
