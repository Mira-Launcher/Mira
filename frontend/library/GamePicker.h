#pragma once

#include <QWidget>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "../components/TileView.h"

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace mira_gui {

class ArtworkStore;
class PickTile;
class ZoomSettle;

// A page of covers to tick, in sections, with a search and a floating bar holding Cancel and the
// page's own apply button. Hidden games wait in their own section, the only one hidden mode
// shows. A picker adds its fields to the header or the bar and says what applying does.
class GamePicker : public QWidget {
  Q_OBJECT

 public:
  // `back` is where its back arrow goes ("Back to Tags").
  GamePicker(ArtworkStore* artwork, const QString& back, QWidget* parent = nullptr);

  // Lets go of the covers once closed, so a restyle doesn't polish a tile per game out of view.
  void Clear();
  void SetTileWidth(int width);
  // The row its tiles snap to fill; room 0 when none show.
  TileRow Row() const;
  // Hidden mode: only the hidden games' section shows, else every other one.
  void SetShowingHidden(bool showing);

 signals:
  void Closed();
  // Ctrl+wheel over the covers, one step per notch.
  void ZoomRequested(int steps);

 protected:
  void resizeEvent(QResizeEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

  // Sets the apply button's text and whether it's enabled; runs on every tick.
  virtual void UpdateApply() = 0;
  virtual void Apply() = 0;

  void SetTitle(const QString& title);
  // Beside the title, and before Cancel in the bar.
  void AddHeaderWidget(QWidget* widget);
  void AddBarWidget(QWidget* widget);
  // After Clear: the sections in order, then the hidden games' (`hidden` true) if any, then
  // FinishSections. `ticked` says which of `games` start ticked.
  void AddSection(const QString& heading, const std::vector<const GameSummary*>& games,
                  const std::vector<std::string>& ticked, bool hidden = false);
  void FinishSections();
  // The apply text, the bar's place or a header field changed.
  void UpdateBar();

  std::vector<std::string> TickedIds() const;
  int TickedCount() const;
  QLineEdit* Search() const { return search_; }
  QPushButton* ApplyButton() const { return apply_; }
  QWidget* BarBox() const { return bar_; }
  ArtworkStore* Artwork() const { return artwork_; }

  // Sorted by name, ignoring case, and split into the shown games and the hidden ones.
  static void SortAndSplit(std::vector<const GameSummary*>& games,
                           std::vector<const GameSummary*>& hidden);

 private:
  static constexpr int kTileGap = 10;
  void Filter();

  ArtworkStore* artwork_ = nullptr;
  ZoomSettle* zoom_settle_ = nullptr;
  QWidget* header_box_ = nullptr;
  QHBoxLayout* header_ = nullptr;
  QLabel* title_ = nullptr;
  QLineEdit* search_ = nullptr;
  QScrollArea* scroll_ = nullptr;
  QWidget* content_ = nullptr;
  QVBoxLayout* sections_ = nullptr;
  QList<PickTile*> tiles_;
  QWidget* hidden_section_ = nullptr;  // its grid; the heading is the item before it
  QWidget* bar_ = nullptr;
  QHBoxLayout* bar_layout_ = nullptr;
  QPushButton* apply_ = nullptr;
  bool show_hidden_ = false;
  int tile_width_ = 120;
};

}  // namespace mira_gui
