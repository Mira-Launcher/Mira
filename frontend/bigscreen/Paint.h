#pragma once

#include <QColor>
#include <QFont>
#include <QPixmap>
#include <QRectF>
#include <QString>

#include <optional>

#include "../client/Types.h"
#include "NavRepeater.h"

class QPainter;

namespace mira_gui::bigscreen {

// A game Mira tracks, or a store title the account owns but hasn't installed.
struct Item {
  QString key;     // the game id, or "<source>-<ref>" for a title (where its art is kept)
  QString name;
  QString source;  // a source id
  QString ref;     // titles only
  std::optional<GameSummary> game;
  bool installed() const { return game.has_value(); }
};

// One button hint: the button and what it does here.
struct Hint {
  Nav nav;
  QString label;
};

// Big screen is laid out in units of 1/49 of the screen height, so a TV and
// the Deck share one layout.
inline double Unit(int height, bool large_text) { return height / 49.0 * (large_text ? 1.15 : 1.0); }

QFont Font(double unit, double size, int weight = QFont::Normal);
// The focus color: picked from the focused game's art, else the theme's accent.
QColor Accent();
void SetAccent(const QColor& color);

// The label shown for `nav` ("A", "✕", "LB", "Enter", ...) in `kind` ("xbox", "ps", "nin", "keys").
QString GlyphText(Nav nav, const QString& kind);
// A round (face) or rounded (shoulder) button glyph; returns its width.
double DrawGlyph(QPainter& painter, QPointF left_center, double unit, Nav nav, const QString& kind);
// How wide DrawGlyph draws it.
double GlyphWidth(double unit, Nav nav, const QString& kind);

// A cover with its focus ring, dimming and install progress.
// A game's name at the top of a page, in at most two lines inside `box` (its width and top): a
// smaller size when it doesn't fit, then cut off. Returns the area used.
QRectF DrawTitle(QPainter& painter, const QRectF& box, double unit, double size, const QString& text);

// Covers (2:3) in rows of `columns` inside `area`: sized so two rows always fit, centered, and
// scrolled so the focused row is whole.
struct CoverGrid {
  CoverGrid(const QRectF& area, int columns, double gap, int focus_row);
  // Where cover `index` goes; rows scrolled past are above `area`.
  QRectF Box(int index) const;
  QSize Tile() const { return {qRound(tile_w), qRound(tile_h)}; }

  QRectF area;
  int columns;
  double gap, tile_w, tile_h, left;
  int first_row;
};

void DrawCover(QPainter& painter, const QRectF& rect, const QPixmap& cover, double unit, bool focused,
               bool dim, double progress);

// A filled pill with a colored dot; returns its width.
double DrawPill(QPainter& painter, QPointF top_left, double unit, const QString& text,
                const std::optional<QColor>& dot = std::nullopt);

// "Ready", "Not installed", "Installing · 42%", "Running"... and its color.
QString StatusText(const Item& item, double progress, bool paused);
QColor StatusColor(const Item& item, double progress);

QString SourceName(const QString& source);

}  // namespace mira_gui::bigscreen
