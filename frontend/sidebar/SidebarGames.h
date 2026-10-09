#pragma once

#include <QPushButton>
#include <QString>

#include <string>
#include <vector>

#include "../client/Types.h"

class QPainter;
class QRectF;

namespace mira_gui {

class ArtworkStore;

// How the sidebar's PINNED and RECENTLY PLAYED sections draw their games
// (frontend.toml's sidebar_pinned_style / sidebar_recent_style).
namespace sidebar {

enum class Style {
  Covers,  // a small cover next to the name
  Hero,    // a short banner of the game's hero art behind its name
  Shelf,   // small covers, four across, no names
};

struct StyleOption {
  Style style;
  const char* key;  // as stored in frontend.toml
  const char* label;
};
// Every style, in the order the customize card lists them.
const std::vector<StyleOption>& StyleOptions();
// Unknown or empty keys are Covers, so an old or hand-edited file still shows rows.
Style ParseStyle(const std::string& key);
const char* StyleKey(Style style);

// One game as a customize-card preview draws it.
struct PreviewGame {
  std::string id;
  QString name;
  QString trailing;        // "Yesterday", "Playing", or empty
  QString short_trailing;  // the same for a shelf cover: "1d ago"
};
// A miniature of a section in `style`, drawn with `games` and their art;
// with none, a sketch in the theme's colors instead.
void PaintStylePreview(QPainter* painter, const QRect& rect, Style style, const std::vector<PreviewGame>& games,
                       ArtworkStore* artwork);

// Shelf columns for `count` covers: as many as there are, two to four.
int ShelfColumns(int count);
// A shelf cover's "Yesterday" or "Playing", over a dark fade at its foot.
void DrawCoverLabel(QPainter* painter, const QRectF& cover, const QString& text, int pixel_size);
// Space between shelf covers.
inline constexpr int kShelfGap = 6;

// Up to three of `games`' covers fanned like a hand of cards, the first on top, as a
// source row leads with. With none, a dashed outline where a card would be.
inline constexpr QSize kDeckSize(50, 46);
QPixmap CoverDeck(const std::vector<const GameSummary*>& games, ArtworkStore* artwork, qreal dpr);

// A Covers row: a flat button whose hover takes the cover's color.
// `trailing` is muted text on the right ("Yesterday", "Playing"), empty for none.
QPushButton* MakeCoverRow(const GameSummary& game, ArtworkStore* artwork, const QString& trailing,
                          QWidget* parent);

// A Hero row: the hero art (or the cover) under a scrim, the name over it.
class HeroRow : public QPushButton {
public:
  HeroRow(const GameSummary& game, ArtworkStore* artwork, const QString& trailing, QWidget* parent);
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  std::string id_;
  bool running_;
  QString trailing_;
  ArtworkStore* artwork_;
  QPixmap scaled_;  // the art at the last painted size
};

// One cover on a Shelf.
class ShelfCover : public QPushButton {
public:
  // `trailing` ("Yesterday", "Playing") is drawn over the cover's foot; empty for none.
  ShelfCover(const GameSummary& game, ArtworkStore* artwork, const QString& trailing, QWidget* parent);

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  QString id_;
  QString name_;
  bool running_;
  QString trailing_;
  ArtworkStore* artwork_;
};

// An empty place in a section, drawn like the customize card's sketch of
// `style`: faded sample art and a bar where the name would be. `index` varies them.
class PlaceholderRow : public QWidget {
 public:
  PlaceholderRow(Style style, int index, QWidget* parent);
  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  Style style_;
  int index_;
};

// Lays its covers (ShelfCovers or Shelf PlaceholderRows) out ShelfColumns
// across at the sidebar's width, so fewer games get bigger covers; the height follows.
class Shelf : public QWidget {
public:
  explicit Shelf(QWidget* parent);
  void Add(QWidget* cover);

  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int width) const override;
  QSize sizeHint() const override;

protected:
  void resizeEvent(QResizeEvent* event) override;

private:
  static constexpr int kGap = kShelfGap;
  int Columns() const;
  int CellWidth(int width) const;
  std::vector<QWidget*> covers_;
};

// Which art a style draws for this game, so a section can tell a row worth
// rebuilding from one that would come out the same.
QString ArtSignature(const GameSummary& game, Style style, ArtworkStore* artwork);

}  // namespace sidebar
}  // namespace mira_gui
