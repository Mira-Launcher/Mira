#pragma once

#include <QPixmap>
#include <QWidget>

#include <string>

#include "../client/Types.h"

class QTimer;

namespace mira_gui {

class ArtworkStore;

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

protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

private:
  void LoadHero();
  QPixmap Source() const;

  QTimer* resize_settled_ = nullptr;
  bool resizing_ = false;

  ArtworkStore* artwork_ = nullptr;
  GameSummary game_;
  QPixmap hero_;
  QPixmap hero_preview_;
  QPixmap cover_preview_;
  // Source() scaled and blurred for the current size; rebuilt on a change.
  QPixmap rendered_;
  qint64 rendered_key_ = 0;
};

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
