#pragma once

#include <QString>
#include <Qt>

class QLabel;
class QWidget;

namespace mira_gui {

// A label in one of base.qss's text roles ("muted", "section", "error", ...),
// or plain with no `role`. Wraps unless told not to.
QLabel* MakeLabel(QWidget* parent, const QString& text, const char* role = nullptr,
                  bool wrap = true);

// A small caps-style group heading ("PINNED", "SORT") in the muted color.
QLabel* MakeGroupHeading(QWidget* parent, const QString& text);

// A size as every label shows one: "1.8 GB", "12.5 MB".
QString SizeText(qint64 bytes);

// A 1 px line in the theme's border color, following theme changes.
// `length` fixes its long side; 0 lets the layout stretch it.
QWidget* MakeDivider(QWidget* parent, Qt::Orientation orientation, int length = 0);

}  // namespace mira_gui
