#pragma once

#include <QWidget>

#include <optional>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "../widgets/TileView.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace mira_gui {

class ArtworkStore;
class PickTile;
class Switch;

// The Tags page's game picker: covers to tick, the games a tag would go on. Sections come first
// (e.g. the games Steam gives the tag, ticked), then every other game unticked, with a search.
// Hidden games wait in their own section that Ctrl+H shows, ticked as they would be elsewhere;
// store launchers aren't offered, and keep the tag as they had it. Applying makes exactly the ticked games have the tag
// (POST /v1/tags/set).
class TagPicker : public QWidget {
  Q_OBJECT

public:
  struct Section {
    QString heading;
    std::vector<std::string> ids;
    bool ticked = false;
  };

  explicit TagPicker(ArtworkStore* artwork, QWidget* parent = nullptr);

  // `name` empty asks for one. `existing` is true for a tag the library has: then Save sends the
  // ticked set as is, and the folder switch reads "A folder". `known` is the library's tags, so a
  // typed name that's already a tag keeps the games that have it.
  void Open(const QString& name, bool existing, std::vector<Section> sections, const std::vector<GameSummary>& games,
            bool folder, std::vector<TagSummary> known);
  void SetTileWidth(int width);
  // The row its tiles snap to fill; room 0 when none show.
  TileRow Row() const;
  void ToggleHidden();

signals:
  void Closed();
  // The tag was set; the records that changed, for the window to apply now.
  void Applied(const std::vector<GameSummary>& games);
  // Ctrl+wheel over the covers, one step per notch.
  void ZoomRequested(int steps);

protected:
  void resizeEvent(QResizeEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  static constexpr int kTileGap = 10;
  // `ticked` says which of `games` start ticked.
  QWidget* AddSection(const QString& heading, const std::vector<const GameSummary*>& games,
                      const std::vector<std::string>& ticked);
  void Filter();
  void UpdateBar();
  void Apply();
  // The games that end with the tag: the ticked ones, and the launchers that had it.
  std::vector<std::string> Chosen() const;

  ArtworkStore* artwork_ = nullptr;
  QLabel* title_ = nullptr;
  QLineEdit* name_edit_ = nullptr;
  QLineEdit* search_ = nullptr;
  QScrollArea* scroll_ = nullptr;
  QWidget* content_ = nullptr;
  QVBoxLayout* sections_ = nullptr;
  QList<PickTile*> tiles_;
  QWidget* hidden_section_ = nullptr;  // its grid; the heading is the item before it
  QWidget* bar_ = nullptr;
  Switch* folder_ = nullptr;
  QLabel* folder_label_ = nullptr;
  QPushButton* apply_ = nullptr;
  QString name_;
  bool existing_ = false;
  bool folder_was_ = false;
  bool show_hidden_ = false;
  int tile_width_ = 120;
  std::vector<std::string> had_;   // the games that had the tag when the picker opened
  std::vector<std::string> kept_;  // the launchers among them, which aren't offered
  std::vector<TagSummary> known_;
};

}  // namespace mira_gui
