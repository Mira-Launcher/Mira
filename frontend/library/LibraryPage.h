#pragma once

#include <QString>
#include <QWidget>
#include <string>
#include <utility>
#include <vector>

#include <functional>
#include <set>

#include "../activity/DownloadTracker.h"
#include "../client/Types.h"
#include "OwnedTitles.h"

class QLabel;
class QLineEdit;
class QModelIndex;
class QScrollArea;
class QStandardItemModel;
class QVBoxLayout;

namespace mira_gui {

class ArtworkStore;
class ContinueRow;
class FilterSortPill;
class GameFilterProxy;
class GameLibraryModel;
class GameTileDelegate;
class LibraryGrid;
class TabRow;
class TileGrid;

// The library's own page: a tab row holding the filter tabs, the filter and
// sort pill and the search box, then the Continue playing cards, then the
// grid of every game the filter and search let through.
//
// The whole library is filtered here on the client, which keeps the search
// instant and lets "Playing now" and "Never played" be filters at all.
class LibraryPage : public QWidget {
  Q_OBJECT

 public:
  LibraryPage(GameLibraryModel* library, ArtworkStore* artwork, const std::string& sort_key,
              bool sort_descending, int tile_width, QWidget* parent = nullptr);

  // What frontend.toml says about the tabs, the Continue row and the tiles.
  void ApplyPrefs(const FrontendPrefs& prefs);

  QString FilterKey() const;
  void SetFilterKey(const QString& key);
  // Ctrl+1…9: the filter in that row of the pill's list.
  void PickFilter(int row);
  int FilterCount() const;
  // Ctrl+H: Hidden, or back to All from it.
  void ToggleHidden();
  const std::string& SortKey() const;
  bool SortDescending() const;

  void FocusSearch();
  // Esc: clears the search, then the selection.
  void ClearSearchOrSelection();
  void ClearSelection();
  // The selected tiles' ids and names, in grid order.
  std::vector<std::pair<std::string, QString>> SelectedGames() const;
  // The one selected tile's id; empty with none or several.
  std::string SelectedId() const;
  // Selects that game alone and scrolls to it; false while it's filtered out.
  bool ShowGame(const std::string& id);
  // Adds that game to the selection, or takes it out; false while it's filtered out.
  bool ToggleSelected(const std::string& id);
  // A few seconds of `text` over that game's tile.
  void ShowTileNote(const std::string& id, const QString& text);

  // The stores' titles a search also looks through, below the library's own matches.
  void SetOwnedTitles(OwnedTitles* titles);
  // Set by the owner: a store title's running install, nullopt while idle.
  std::function<std::optional<DownloadTracker::TileProgress>(const QString& source, const QString& ref)> title_progress;
  // Redraws the store matches' install states.
  void RefreshOwnedStates();
  int ShownCount() const;

  int TileWidth() const { return tile_width_; }
  void SetTileWidth(int width);
  // A new cover for `id`, for the Continue row (the tiles follow the model).
  void UpdateCover(const std::string& id);
  // The filter pill and the search box, which mean nothing while the grid is covered.
  void SetControlsEnabled(bool enabled);
  // Where the grid's own shortcuts live, so a text field keeps its keys.
  QWidget* ShortcutScope() const;

 signals:
  void FilterChanged();
  void SortChanged();
  // What the grid shows changed: the filter, the search, or the library.
  void ShownChanged();
  void SelectionChanged();
  void GameActivated(const std::string& id);  // a tile's double click
  // Install pressed on a store match (the store picked, when several sell it).
  void InstallTitleRequested(const QString& source, const QString& ref);
  void PlayRequested(const std::string& id);  // a Continue playing card
  void GameMenuRequested(const std::string& id, const QPoint& global_pos);
  void BatchMenuRequested(const std::vector<std::string>& ids, const QPoint& global_pos);
  // A tile's hover card after its dwell (`anchor` is global), and its end.
  void HoverRequested(const std::string& id, const QRect& anchor);
  void HoverEnded();
  // Ctrl+wheel over the grid, one step per notch, positive to grow.
  void ZoomStepped(int steps);

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  QWidget* BuildHeader(const std::string& sort_key, bool sort_descending);
  void ApplyFilter();
  void LibraryChanged();
  void UpdateCounts();
  void UpdateEmptyState();
  void RefreshContinue();
  void ShowContextMenu(const QPoint& pos);
  void Hover(const QModelIndex& index);
  QSize TileSize() const;
  void ApplyLayoutTokens();
  QWidget* BuildOwnedSection();
  // Lists the store titles matching the search, and sizes the grid for it.
  void UpdateOwnedMatches();
  // While store matches show under it, the grid is as tall as its tiles and the page scrolls.
  void FitGrid();
  void InstallMatch(const QModelIndex& index, const QPoint& global_pos);

  GameLibraryModel* library_ = nullptr;
  ArtworkStore* artwork_ = nullptr;
  GameFilterProxy* games_ = nullptr;
  TabRow* tabs_ = nullptr;
  FilterSortPill* pill_ = nullptr;
  QLineEdit* search_ = nullptr;
  ContinueRow* continue_row_ = nullptr;
  LibraryGrid* grid_ = nullptr;
  GameTileDelegate* delegate_ = nullptr;
  QLabel* empty_hint_ = nullptr;
  QScrollArea* scroll_ = nullptr;  // scrolls only while store matches show
  QVBoxLayout* content_layout_ = nullptr;
  QWidget* owned_section_ = nullptr;
  QLabel* owned_heading_ = nullptr;
  TileGrid* owned_grid_ = nullptr;
  QStandardItemModel* owned_model_ = nullptr;
  OwnedTitles* owned_titles_ = nullptr;
  std::vector<OwnedMatch> owned_matches_;
  std::set<QString> covers_asked_;  // "<source>-<ref>" whose cover fetch was queued
  int tile_width_ = 0;
  bool continue_row_enabled_ = true;
  int continue_count_ = 3;
  bool continue_apps_ = false;
  bool apps_in_all_ = true;
};

}  // namespace mira_gui
