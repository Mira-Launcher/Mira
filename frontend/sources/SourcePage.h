#pragma once

#include <QPoint>
#include <QHash>
#include <QSet>
#include <QSize>
#include <QString>
#include <QWidget>

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "../client/Types.h"
#include "Sources.h"

class QLabel;
class QLineEdit;
class QMenu;
class QModelIndex;
class QStandardItemModel;
class QPushButton;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

namespace mira_gui {

class ArtworkStore;
class DownloadTracker;
class GameFilterProxy;
class GameLibraryModel;
class HoverCard;
class ModalOverlay;
class SourceSettingsCard;
class SourceSetupCard;
class TabRow;
class TileGrid;

// One store, launcher or other program's page in the library window: the
// games that came from it as cover tiles, whatever setup it still needs, and
// (stores) what the account owns but hasn't installed. Rebuilt on every
// open, so it starts from mirad's current state.
class SourcePage : public QWidget {
  Q_OBJECT

public:
  // `library` is the window's shared game model; the page shows this source's games from it.
  // `tabs`: installed and not installed games as tabs, else stacked.
  SourcePage(const SourceInfo& source, GameLibraryModel* library, ArtworkStore* artwork, DownloadTracker* downloads,
             bool tabs, int tile_width, QWidget* parent = nullptr);

  // A store title's cover arrived; a game's own tile repaints from the model.
  void UpdateCover(const QString& id);
  void SetTileWidth(int width);
  void SetDragSelectEnabled(bool enabled);
  // A few seconds of `text` over that game's tile in "In your library".
  void ShowTileNote(const QString& id, const QString& text);
  // Starts an update of an installed store title.
  void UpdateTitle(const QString& ref);
  // Null until the banner's settings button first opens it.
  SourceSettingsCard* SettingsCard() const { return settings_card_; }
  bool SettingsModalOpen() const;
  // Esc: closes the settings dialog, asking first when it has unsaved edits.
  void CloseSettingsModal();

signals:
  // An import added or changed games.
  void LibraryChanged();
  void OpenSettingsRequested(QString focus_key);
  void PlayRequested(QString id);
  // Right-click on an installed game: the library's own game menu, plus
  // Update when `update_ref` is set.
  void GameMenuRequested(QString id, QPoint global_pos, QString update_ref);
  // The source was removed from the header's menu; the page should close.
  void Removed();
  // Right-click on several selected installed games.
  void BatchMenuRequested(QStringList ids, QPoint global_pos);
  // Ctrl+wheel over a grid, one step per notch; the window owns the zoom.
  void ZoomRequested(int steps);

protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

private:
  bool IsStore() const { return source_.kind == SourceInfo::Kind::Store; }
  bool IsLauncher() const { return source_.kind == SourceInfo::Kind::Launcher; }
  bool HasImport() const;
  bool HasOwned() const;
  // The source has a list of what the account owns, as opposed to a note saying it has none.
  bool ListsOwned() const;
  bool IsOwnGame(const GameSummary& game) const;

  QWidget* BuildTopRow();
  QWidget* BuildLibrarySection();
  QWidget* BuildOwnedSection();

  void OpenSettingsModal();
  void FitSettingsModal();
  void FillMoreMenu(QMenu* menu);
  void UpdateTool();

  void RefreshStatus();
  void ApplyStoreStatus(const StoreStatusResult& status);
  void ApplyLauncher(const LauncherInfo& launcher);
  void UpdateStatusLine();
  // Which of the two sections shows, per the tabs and the account.
  void UpdateSections();
  void Import();
  void RefreshOwned();
  void ShowOwned(const StoreLibraryResult& result);
  void ShowBundles(const HumbleLibraryResult& result);
  void RebuildOwnedTiles();
  void StartInstall(const QString& ref, bool update);
  void ApplyFilter();
  void ShowLibraryMenu(const QPoint& pos);
  void ShowOwnedMenu(const QPoint& pos);
  // nullptr hides it.
  void ShowHoverCard(TileGrid* grid, const QModelIndex& index);
  // The shared model changed: this source's count and status line.
  void LibraryUpdated();
  void HandleEvent(const std::string& type, const std::string& data);

  SourceInfo source_;
  std::string id_;
  ArtworkStore* artwork_ = nullptr;
  DownloadTracker* downloads_ = nullptr;
  bool tool_installed_ = false;
  bool authenticated_ = false;
  bool launcher_installed_ = false;
  bool launcher_installing_ = false;
  bool tool_updating_ = false;
  std::string tool_version_;
  std::string launcher_game_id_;
  std::string launcher_prefix_;
  std::string account_;
  int library_count_ = 0;
  QSize tile_;
  bool use_tabs_ = true;
  bool owned_available_ = false;  // the account is ready to list what it owns

  QLabel* status_line_ = nullptr;
  QPushButton* banner_primary_ = nullptr;  // Open launcher / Sign out
  TabRow* tabs_ = nullptr;
  QLineEdit* filter_ = nullptr;
  QToolButton* settings_button_ = nullptr;
  QToolButton* more_button_ = nullptr;
  QWidget* content_ = nullptr;
  QVBoxLayout* content_layout_ = nullptr;
  SourceSettingsCard* settings_card_ = nullptr;  // built on first open
  ModalOverlay* settings_overlay_ = nullptr;
  QScrollArea* settings_scroll_ = nullptr;

  SourceSetupCard* setup_card_ = nullptr;

  QWidget* library_section_ = nullptr;
  QLabel* library_heading_ = nullptr;
  QPushButton* import_button_ = nullptr;
  QLabel* import_result_ = nullptr;
  TileGrid* library_grid_ = nullptr;
  QLabel* library_empty_ = nullptr;

  QWidget* owned_section_ = nullptr;
  QLabel* owned_heading_ = nullptr;
  QPushButton* owned_refresh_ = nullptr;
  QLabel* owned_note_ = nullptr;
  QPushButton* steam_settings_ = nullptr;
  QPushButton* art_key_ = nullptr;  // covers need a SteamGridDB key
  QString art_key_setting_;         // that failure's fix target
  TileGrid* owned_grid_ = nullptr;

  // What the account owns and isn't installed, by ref, with its title.
  std::vector<std::pair<QString, QString>> owned_;
  QSet<QString> not_owned_;  // refs listed from a collection but not bought
  QHash<QString, QString> tiers_;  // ref -> ProtonDB tier
  QHash<QString, QStringList> tags_;  // ref -> Steam tags, for the filter
  // Refs asked to install/update/download and not yet started by mirad, and
  // the ones done this session. What's running comes from downloads_.
  QHash<QString, QString> owned_state_;  // ref -> "Installing…", "Downloaded", ...
  QHash<QString, QString> humble_paths_;  // downloaded bundle key -> its folder, to add as a game

  GameLibraryModel* library_ = nullptr;
  GameFilterProxy* games_ = nullptr;        // this source's games, for library_grid_
  QStandardItemModel* owned_model_ = nullptr;  // owned_ as tiles, for owned_grid_
  HoverCard* hover_card_ = nullptr;
};

}  // namespace mira_gui
