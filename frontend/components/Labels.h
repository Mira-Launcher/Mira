#pragma once

#include <QColor>
#include <QLabel>
#include <QString>
#include <QStringList>
#include <Qt>

#include "Text.h"

class QWidget;

namespace mira_gui {

// A label in `style` (components/Text). Wraps unless told not to.
QLabel* MakeLabel(QWidget* parent, const QString& text, TextStyle style = text::kBody,
                  bool wrap = true);

// A small caps-style group heading ("PINNED", "SORT") in the muted color.
QLabel* MakeGroupHeading(QWidget* parent, const QString& text);

// A small outlined pill beside a name: "Beta", "Not checked". `tone` colors it: "accent",
// "warning", or muted.
QLabel* MakeTag(QWidget* parent, const QString& text, const char* tone = nullptr);

// "a, b and c".
QString JoinAnd(QStringList items);

// "1 game", "3 games".
QString GamesCount(qsizetype count);
// "43 of 45 games", "2 of 2 apps", "45 of 47 games and apps": `shown` out of a set of `games`
// games and `apps` apps, named for what's in it.
QString ShownOf(qsizetype shown, qsizetype games, qsizetype apps);

// A size as every label shows one: "1.8 GB", "12.5 MB".
QString SizeText(qint64 bytes);

// A path as labels show it: "~/Mira/Games" for one in the home folder.
QString HomeRelative(const QString& path);

// A "~" or "~/..." path with the home folder written out; any other path as it is.
QString ExpandHome(const QString& path);

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
