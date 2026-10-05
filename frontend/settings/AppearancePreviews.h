#pragma once

#include <QAbstractButton>
#include <QWidget>

#include <vector>

#include "../client/Types.h"
#include "../theme/Theme.h"

class QStandardItemModel;

namespace mira_gui {

class ArtworkStore;
class GameTileDelegate;

// A tile previewing one theme: a small Mira window in its colors, showing a
// few of the user's covers. "auto" draws the dark and light themes split.
class ThemeChoice : public QAbstractButton {
  Q_OBJECT

public:
  ThemeChoice(const QString& theme, const QString& label, std::vector<GameSummary> games, ArtworkStore* artwork,
              QWidget* parent = nullptr);
  QString Theme() const { return theme_; }

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  // Read from the theme files once, not on every paint.
  void LoadTokens();

  QString theme_;
  QString label_;
  std::vector<GameSummary> games_;
  ArtworkStore* artwork_;
  theme::Tokens tokens_;  // theme_'s, or the dark half for "auto"
  theme::Tokens light_;   // the light half for "auto"
};

// Two of the user's games drawn by the library's own tile painter, so the
// status, source mark and pin badge toggles show exactly what they change.
class TilePreview : public QWidget {
  Q_OBJECT

public:
  TilePreview(std::vector<GameSummary> games, ArtworkStore* artwork, QWidget* parent = nullptr);
  void SetShown(bool status, bool source_mark, bool pin_badge);
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  QStandardItemModel* model_;
  bool samples_ = false;  // standing in for games the user doesn't have yet
  GameTileDelegate* delegate_;
};

// A strip of covers in a panel, drawn with the layout values being edited
// rather than the applied theme's, so a change shows before it is saved.
class LayoutPreview : public QWidget {
  Q_OBJECT

public:
  struct Shape {
    int tile_gap = 5;
    int grid_padding = 6;
    int cover_radius = 6;
    int panel_radius = 8;
    int control_radius = 6;
  };

  LayoutPreview(std::vector<GameSummary> games, ArtworkStore* artwork, QWidget* parent = nullptr);
  void SetShape(const Shape& shape);
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent* event) override;

private:
  std::vector<GameSummary> games_;
  ArtworkStore* artwork_;
  Shape shape_;
};

}  // namespace mira_gui
