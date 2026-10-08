#pragma once

#include <QWidget>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../client/Types.h"

class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTimer;

namespace mira_gui {

class ArtworkStore;
class ChangeBar;
class CountPill;
class ElidedLabel;
class RowColumns;
class SettingRow;
class SettingsCard;
class SteamTagRow;
class Switch;
class TagPicker;

// The library window's Tags page: the library's tags with how many games have each, folder tags
// first in the order that picks a game's folder, the Steam tags on its games to add as tags, and
// the tag settings. Your tags and the settings share a column only as wide as they need, Steam's
// tags fill the rest in as many columns as fit. The cards scroll inside, so the settings stay in view. Rows
// are updated in place as the tags change. Adding or choosing games opens a picker in place of
// the cards. Changes that move folders say how many first, in a bar at the bottom.
class TagsPage : public QWidget {
  Q_OBJECT

public:
  explicit TagsPage(ArtworkStore* artwork, QWidget* parent = nullptr);

  // The whole library, for the picker and the library folders' names.
  void SetGames(const std::vector<GameSummary>& games);
  // The picker's covers, as wide as the library's tiles.
  void SetTileWidth(int width);
  bool PickerOpen() const;
  // Ctrl+H while the picker is open: its hidden games show or not.
  void ToggleHidden();
  // The window is leaving the page, which it keeps: a rename being typed, an open picker and a
  // question in the bar are all dropped, as Esc or Cancel would.
  void Leave();

signals:
  // Records a request changed, for the window to apply without waiting for their events.
  void GamesChanged(const std::vector<GameSummary>& games);
  // Ctrl+wheel over the picker's covers, one step per notch; the window owns the zoom.
  void ZoomRequested(int steps);
  void PickerToggled();

protected:
  void resizeEvent(QResizeEvent* event) override;

private:
  struct Settings {
    std::vector<std::string> roots;  // library_roots as written
    std::vector<std::string> sorted_roots;
    std::vector<std::string> folders;  // in the order that picks a game's folder
    bool steam = true;
    bool steam_by_name = true;
    bool tag_by_root = true;
  };
  // One of Your tags' rows, kept across refreshes and updated in place.
  struct MineRow {
    SettingRow* row = nullptr;
    QLabel* icon = nullptr;
    QLabel* count = nullptr;
    Switch* folder = nullptr;
  };
  struct SteamRow {
    SteamTagRow* row = nullptr;
    CountPill* count = nullptr;
    std::size_t games = 0;
  };

  void Refresh();
  void RefreshSoon();
  void Sync();
  void SyncMine();
  void SyncSteam();
  void FilterSteam();
  void SyncSettings();
  void Arrange();
  // The left column's width, and Your tags' least height, from their rows.
  void FitLeft();
  MineRow& MineRowFor(const std::string& name);
  SteamRow& SteamRowFor(const std::string& name);
  void OpenPicker(const QString& name, bool existing);
  void ShowMenu(const std::string& name, QWidget* anchor);
  void StartRename(const std::string& name);
  // Renames `from` everywhere, after asking in the bar when folders would move, and always before
  // a merge into a tag the library already has.
  void Rename(const std::string& from, const std::string& to);
  void FoldersReordered();
  // Asks in the bar before a change that would move folders, after the preview says how many;
  // applies at once when none would, unless `always_ask` (a change that can't be undone).
  // `question` takes the count; `tags` are games' tags as the change would leave them.
  void AskThenApply(std::optional<std::vector<std::string>> folders,
                    std::optional<std::vector<std::string>> sorted_roots, std::function<QString(int)> question,
                    const QString& apply_label, std::function<void()> apply,
                    const std::map<std::string, std::vector<std::string>>& tags = {}, bool always_ask = false);
  void SetFolder(const std::string& name, bool folder);
  void SetSorted(const std::string& root, bool sorted);
  void PatchSetting(const std::string& key, const std::string& type, const std::string& value);
  void Failed(const QString& what, const ApiError& error);
  const TagSummary* Mine(const std::string& name) const;

  ArtworkStore* artwork_ = nullptr;
  std::vector<GameSummary> games_;
  TagsResult tags_;
  Settings settings_;
  bool fetching_steam_ = false;
  bool tags_loaded_ = false;
  bool settings_loaded_ = false;
  bool show_rare_steam_ = false;  // the Steam tags on only one game, which are many and say little
  int folder_width_ = 0;          // the Folder column's, as wide as its heading or a switch

  QStackedWidget* stack_ = nullptr;
  QWidget* overview_ = nullptr;
  QGridLayout* grid_ = nullptr;
  QWidget* left_ = nullptr;  // Your tags over the tag settings
  SettingsCard* mine_ = nullptr;
  SettingsCard* steam_ = nullptr;
  SettingsCard* settings_card_ = nullptr;
  std::map<std::string, MineRow> mine_rows_;
  SettingRow* mine_note_ = nullptr;
  std::map<std::string, SteamRow> steam_rows_;
  RowColumns* steam_columns_ = nullptr;  // the Steam rows, in as many columns as fit
  SettingRow* steam_note_ = nullptr;
  QPushButton* steam_rare_ = nullptr;  // shows or hides the Steam tags on one game
  SettingRow* steam_rare_row_ = nullptr;
  std::map<std::string, Switch*> setting_switches_;  // by setting key, or "sort:<root>"
  std::vector<std::string> settings_roots_;          // the roots the settings rows were made for
  QLineEdit* steam_search_ = nullptr;
  TagPicker* picker_ = nullptr;
  ChangeBar* bar_ = nullptr;
  std::function<void()> bar_apply_;  // what the bar's primary button does while it asks
  std::function<void()> end_rename_;  // ends the open rename's edit, if one is still open
  int asked_ = 0;                    // so a slow preview never answers a newer question
  QTimer* refresh_timer_ = nullptr;
};

}  // namespace mira_gui
