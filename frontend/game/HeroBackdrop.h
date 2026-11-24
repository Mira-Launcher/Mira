#pragma once

#include <QMargins>
#include <QPainterPath>
#include <QPixmap>
#include <QWidget>
#include <string>

#include "../client/Types.h"

class QTimer;

namespace mira_gui {

class ArtworkStore;

// The game card's content inset from its sides: the header, tabs, chips, settings cards and
// previews of every page line up on it.
constexpr int kCardInset = 24;

// A card page's header row (Back plus title), on the inset for the back arrow's glyph rather than
// its button, whose padding is 6, and the page's content under it.
constexpr QMargins kPageHeaderMargins{kCardInset - 6, 12, kCardInset, 4};
constexpr QMargins kPageContentMargins{kCardInset, 10, kCardInset, kCardInset};

// A card whose top is a game's hero art, darkening into the card's surface
// so whatever is laid over it stays readable. A game with no hero gets its
// cover, blurred; one with neither, its tile's generated art, blurred.
class HeroBackdrop : public QWidget {
  Q_OBJECT

 public:
  explicit HeroBackdrop(ArtworkStore* artwork, QWidget* parent = nullptr);

  void ShowGame(const GameSummary& game);
  // A new cover or hero for `id` (safe to call for any game).
  void RefreshCover(const std::string& id);
  void RefreshHero(const std::string& id);
  // Shown in place of the "hero" or "cover" art until cleared with a null
  // pixmap, or until that slot's new art arrives.
  void SetPreview(const QString& slot, const QPixmap& preview);
  // `base`, a theme color the card is filled with, taking on a little of the art's color.
  QColor Tinted(const QColor& base) const;

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  // The card's rounded outline, inside its 1 px border.
  QPainterPath Outline() const;
  // The art to draw: a picked preview, the hero, else the cover (small) or the tile's art.
  QPixmap Source() const;
  // Whether Source() is a hero, which is drawn as it is, rather than a cover, which is blurred.
  bool ShowsHero() const { return !hero_.isNull() || !hero_preview_.isNull(); }
  // While the card is being resized, art is scaled quickly; smoothly once it stops.
  bool Resizing() const { return resizing_; }
  // The art's dominant color, which Tinted mixes in.
  void UpdateTone(const QPixmap& source);

 private:
  void LoadHero();

  QTimer* resize_settled_ = nullptr;
  bool resizing_ = false;

  ArtworkStore* artwork_ = nullptr;
  GameSummary game_;
  QPixmap hero_;
  QPixmap hero_preview_;
  QPixmap cover_preview_;
  // Source() scaled and blurred for the current size; rebuilt on a change.
  QPixmap rendered_;
  mutable QPixmap placeholder_;  // made once per game, so its blur is cached too
  qint64 rendered_key_ = 0;
  QColor tone_;  // DominantColor of Source(), at tone_key_
  qint64 tone_key_ = 0;
};

// The game card's backdrop: a blurred wash of the hero (or the cover) over the whole card, the
// sharp hero fading out from the top-left corner behind the panel, and a scrim of the window's
// color, tinted with the art's, over both so text stays readable. Each layer is made once per size
// and art, as the card opens or after a resize, never on a timer.
class CardBackdrop : public HeroBackdrop {
  Q_OBJECT

 public:
  using HeroBackdrop::HeroBackdrop;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  QPixmap wash_;
  qint64 wash_key_ = 0;
  QPixmap sharp_;  // the hero, masked to fade out from its top-left corner
  qint64 sharp_key_ = 0;
};

// A hero card's lower part, out to the card's edges (inside its border): clear at its top and the
// window's color (tinted as its HeroBackdrop parent tints) at 82% further down, so the hero runs
// into it with no edge and still shows through behind what it holds. It has the card's bottom
// corners.
class HeroPanel : public QWidget {
  Q_OBJECT

 public:
  explicit HeroPanel(QWidget* parent = nullptr);

 protected:
  void paintEvent(QPaintEvent* event) override;
};

// Fades in `widget`'s top few pixels, inside its content's own top margin, so what scrolls up
// under a HeroPanel's top dissolves rather than ending on a line.
void FadeTop(QWidget* widget);

// A game's cover at a small fixed size, for a header.
class CoverChip : public QWidget {
  Q_OBJECT

 public:
  explicit CoverChip(ArtworkStore* artwork, QWidget* parent = nullptr);

  void ShowGame(const GameSummary& game);
  // Same rules as HeroBackdrop's.
  void RefreshCover(const std::string& id);
  void SetPreview(const QPixmap& preview);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  ArtworkStore* artwork_ = nullptr;
  GameSummary game_;
  QPixmap preview_;
  QPixmap preview_filled_;  // preview_ scaled to the chip at preview_dpr_, cached across paints
  qreal preview_dpr_ = 0;
};

}  // namespace mira_gui
