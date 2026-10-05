#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMainWindow>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QSize>
#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <deque>
#include <vector>

#include "../client/Types.h"
#include "../ui/ArtworkStore.h"
#include "../ui/InstallPromptCard.h"
#include "../ui/InstallerCards.h"
#include "../ui/ManageSourcesCard.h"
#include "../ui/Shortcuts.h"
#include "../ui/SidebarGames.h"

class QLabel;
class QMenu;
class QLineEdit;
class QGridLayout;
class QListWidget;
class QPushButton;
class QSlider;
class QSplitter;
class QStackedLayout;
class QStackedWidget;
class QVBoxLayout;
class QAction;
class QToolButton;
class QListWidgetItem;
class QModelIndex;
class QTimer;

// QListWidget with setViewportMargins made public: Qt keeps it protected on
// QAbstractScrollArea. Defined in LibraryWindow.cpp; this file only ever
// holds a pointer to it.
class LibraryGrid;

namespace mira_gui {
class ArtPickerPanel;
class ChangeBar;
class ContinueRow;
class GameFilterProxy;
class GameLibraryModel;
class CoverChip;
class DaemonSupervisor;
class DownloadTracker;
class DownloadsPanel;
class GameEditForm;
class GameTileDelegate;
class HeroBackdrop;
class HoverCard;
class RunnersPage;
class SettingsPanel;
class SourcePage;
class TabRow;
struct SourceInfo;
}

// Primary library view: cover-art grid, a left sidebar (filters, sort,
// search, Library/Runners nav, Settings), custom top bar in place of a
// native titlebar. Frameless, so it owns its own
// move/resize/minimize/maximize/close.
//
// Selection model: one click selects a tile, a second (double) click
// launches, right-click opens the per-game menu. Hovering a tile shows a
// HoverCard after a short dwell: the tile itself plus the right-click menu
// and the per-game edit page cover everything the old right sidebar used to.
class LibraryWindow : public QMainWindow {
  Q_OBJECT

public:
  // `prefs` is frontend.toml as read at startup; the theme is already applied.
  explicit LibraryWindow(const mira_gui::FrontendPrefs& prefs, QWidget* parent = nullptr);

private:
  QWidget* BuildTopBar();
  QWidget* BuildSidebar();
  QWidget* BuildGrid();
  QWidget* BuildLibraryHeader();
  QWidget* BuildSettingsPage();
  // The sidebar's single filter+sort control, a Qt::Popup so it dismisses
  // itself on an outside click or Escape, so no manual close-on-click-away
  // wiring needed. Built once; filters_ and the sort buttons live inside it.
  QWidget* BuildFilterSortPopover();
  // Refreshes the pill's own summary text/icons after a filter, sort, or
  // theme change; the popover's own rows restyle themselves separately.
  void UpdateFilterSortSummary();
  // Library-only actions as vertical icon+label rows. Refresh/Shortcuts/
  // About moved to the top bar; Close window/Quit dropped (the frameless ×
  // and the tray icon already cover them).
  void BuildShortcuts();

  // Frontend's own state (size, tile size, which filter) round-trips through
  // frontend.toml, not settings.toml. The window's own layout is read in the
  // constructor; these are what the settings screen also changes.
  void ApplySettingsPrefs(const mira_gui::FrontendPrefs& prefs);
  // The layout this window owns (size, zoom, filter, sort, sidebar), saved
  // in the background a moment after it last changed, so a crash loses at
  // most that moment. FlushPrefs writes a pending one now, for quitting.
  mira_gui::FrontendPrefs LayoutPrefs() const;
  void ScheduleSavePrefs();
  void FlushPrefs();
  void resizeEvent(QResizeEvent* event) override;
  void closeEvent(QCloseEvent* event) override;
  void QuitOrClose();
  void changeEvent(QEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  void ToggleMaximize();
  // Redrawn rather than stored: each glyph is painted in the theme's text
  // color, so a theme change has to regenerate them.
  void ApplyTopBarIcons();
  void ApplyLayoutTokens();

  // Lists the library and scans it at once; the scan's changes arrive as
  // events. `force_scan` is Refresh's: startup honours scan_on_startup.
  void Reload(bool force_scan);
  void RefreshGames();
  void ConnectionChanged(bool connected);

  // library_ is the whole library as last heard; the grid shows it
  // through its proxy. Filtering client-side keeps the search box instant
  // and lets "Playing now"/"Never played" be filters at all.
  void ApplyFilter();  // the filter key and search, into the grid's proxy
  void ApplySort();    // the sidebar's sort, into the grid's proxy
  // After any change to library_: counts, footer, sidebar rows, source rows.
  void LibraryChanged();
  void UpdateFilterCounts();
  void UpdateEmptyState();
  void UpdateFooter();
  QString CurrentFilterKey() const;
  void UpsertGames(const std::vector<mira_gui::GameSummary>& games);
  void RemoveGame(const std::string& id);
  const mira_gui::GameSummary* FindGame(const std::string& id) const;

  // The library grid's.
  void SetTileWidth(int width);
  // The slider moved: resizes whichever page is showing.
  void Zoom(int width);
  int SourceTileWidth(const QString& id) const;
  // Points the slider at the page on screen, and off where there's no grid.
  void SyncZoom();
  bool SourcePageShown() const;
  QSize TileSize() const;

  void ClearGridSelection();
  // The selected tiles' ids and names, in grid order.
  std::vector<std::pair<std::string, QString>> SelectedGames() const;
  // The one selected tile's id; empty with none or several.
  std::string SelectedId() const;
  // An invalid index hides it; otherwise positions and fills a persistent
  // HoverCard for that tile. Called by LibraryGrid::on_hover after its dwell.
  void ShowHoverCard(const QModelIndex& index);
  // `anchor` is global; the card goes beside it.
  void ShowHoverCardFor(const mira_gui::GameSummary& game, const QRect& anchor,
                        const QString& hint = QString());
  void ShowContextMenu(const QPoint& pos);
  // `extra` adds entries after Play (e.g. a store's Update).
  void ShowGameMenu(const std::string& id, const QPoint& global_pos,
                    const std::function<void(QMenu&)>& extra = nullptr);
  void ShowSidebarMenu(const QPoint& global_pos);
  void ShowSourceMenu(const mira_gui::SourceInfo& source, const QPoint& global_pos);
  // More than one tile selected: a reduced set of actions applied to all
  // of them at once, chosen at the pos the right-click landed on.
  void ShowBatchMenu(const std::vector<std::string>& ids, const QPoint& global_pos);
  void ToggleRunning(const std::string& id);
  // Adds or removes `tag` ("hidden", "favorite") on one game.
  void ToggleTag(const std::string& id, const std::string& tag);
  // Adds (`present`) or removes `tag` on each id in one request.
  void BatchSetTag(const std::vector<std::string>& ids, const std::string& tag, bool present);
  // game.install_detected: offers to switch a game that was an installer to what it installed.
  void AskAboutInstall(const mira_gui::InstallDetectedEvent& event);
  void ShowInstallPrompt(const mira_gui::InstallDetectedEvent& event);
  // The card that runs a needs_install game's installer.
  void OfferInstall(const std::string& id);
  // The card that offers to delete an installer folder an install left behind.
  void OfferInstallerDelete(const mira_gui::InstallerLeftoverEvent& event);
  // Shows a prompt card now, or once Settings, a game's card and other cards have closed.
  // One per `key`; a second ask while it waits is dropped.
  void QueueCard(const std::string& key, std::function<void()> show);
  void ShowNextCard();
  void LaunchGame(const std::string& id);
  void OpenGameDialog(const std::string& id);
  // Scrim + centered card slot, built once. Shown/hidden per open rather
  // than swapped into content_stack_, so the grid and sidebar stay live
  // underneath it.
  QWidget* BuildGameEditOverlay();
  // The card's own content, rebuilt fresh on every open, for the same reasoning as
  // settings_page_: starts synced to what's actually saved, not stale edits
  // from a discarded previous open.
  QWidget* BuildGameEditCard(const std::string& id);
  void SizeGameEditCard(QWidget* card);
  void CloseGameEdit();
  // Confirms first if game_edit_form_ is dirty; the card's own Back
  // button, the sidebar's Library nav row, and a click on the scrim.
  void RequestCloseGameEdit();
  // The card's Back and Esc: out of the art picker, then Advanced, then the card.
  void GameEditBack();
  // The form's change count, and room under its cards while the bar shows.
  void UpdateGameEditBar();
  // Play or Stop, as the game's state allows.
  void UpdateGameEditPlay();
  bool GameEditOpen() const;
  // `focus_key` jumps straight to that schema field once loaded.
  void OpenSettings(const QString& focus_key = QString());
  void CloseSettings();
  // Confirms first if settings_panel_ is dirty; the settings page's own
  // Back button.
  void RequestCloseSettings();
  bool SettingsOpen() const;
  // Greys out the library controls behind Settings while it's open.
  void SetSettingsChromeVisible(bool settings_open);
  // Shared by SetSettingsChromeVisible and the per-game edit page: neither
  // filtering nor sorting means anything while the grid isn't on screen.
  void SetGridControlsEnabled(bool enabled);
  // Highlights the sidebar's "Library" row exactly when the grid is the
  // visible content (not Settings, not a game's edit page).
  void UpdateLibraryNavActive();
  void ShowLibrary();
  void OpenManageSources();
  // A source was removed: drop its games and turn its sidebar row off.
  void ForgetSource(const QString& id);
  // The sidebar's PINNED and RECENTLY PLAYED rows.
  void RefreshSidebarGames();
  // What PINNED lists, and what RECENTLY PLAYED would list showing `count`
  // (with `running_counts`, running games are part of the count, as on a shelf).
  std::vector<const mira_gui::GameSummary*> PinnedGames() const;
  std::vector<const mira_gui::GameSummary*> RecentGames(int count, bool running_counts = false) const;
  // A row or cover's click, menu and hover card.
  void WireSidebarGame(QPushButton* row, const mira_gui::GameSummary& game);
  // Rebuilds one section's rows in `style`, only if what they'd show differs
  // from `signature`. `recent` rows say when each was last played.
  void FillSidebarSection(QWidget* heading, QVBoxLayout* layout,
                          const std::vector<const mira_gui::GameSummary*>& games, mira_gui::sidebar::Style style,
                          bool recent, QString& signature);
  // A card that changes the sidebar (the pinned and recently played style,
  // Manage sources), over the content with the sidebar left undimmed as its
  // preview. Showing one replaces any other.
  QWidget* BuildSidebarCardOverlay();
  void ShowSidebarCard(QWidget* card);
  void CloseSidebarCard();
  bool SidebarCardOpen() const;
  void OpenSidebarStyle();
  // Redraws both sections and stores their styles and the recent count.
  void SaveSidebarStyle();
  void RefreshContinue();
  // A sidebar row or card's click. Ignores the second click of a double
  // click, which would otherwise land on whatever row moved under it.
  void RowClicked(const std::string& id);
  QIcon SourceIcon(const mira_gui::SourceInfo& source, bool active) const;
  void SetSourceHidden(const QString& id, bool hidden);
  std::vector<QString> SourceOrder() const;
  std::vector<mira_gui::ManageSourcesCard::Entry> SourceEntries() const;
  // Shows and stores a new sidebar order.
  void SetSourceOrder(std::vector<QString> order);
  void NoteImported(const QString& id);
  // Moves `id` to just before the visible row `before` (end if -1).
  void MoveSource(const QString& id, int before);
  int SourceDropRow(int y) const;
  // Closes Settings and a game's card, asking first if either has unsaved
  // edits. False while one stays open.
  bool LeaveOverlays();
  // The Runners page, in the grid's place like a source page.
  void OpenRunners();
  void CloseRunners();
  void OpenAbout();
  void OpenGameDetailPage(const std::string& id);
  void ScanLibrary();
  void ImportSteamLibrary();
  void ImportLutrisLibrary();
  void ImportDesktopEntries();
  void AddGameManually();
  // A store or launcher's page, rebuilt fresh on each open.
  void OpenSource(const mira_gui::SourceInfo& source);
  // False while the page stays: its settings card's edits were kept, or are
  // saving first, and then `retry` runs (CloseSource itself when empty).
  bool CloseSource(std::function<void()> retry = {});
  // Asks about the open source page's unsaved settings; CloseSource's rules.
  bool ConfirmLeaveSource(std::function<void()> retry);
  // Hides the sources turned off in Settings (`<id>.enabled`), and asks
  // which stores are signed in and which launchers installed.
  void RefreshSourceNavs();
  // Greys out and moves down the sources with nothing set up yet.
  void UpdateSourceNavs();
  void SetSourceControlsEnabled(bool enabled);
  // The grid is what's on screen: not Settings, Runners, or a source page.
  bool GridShown() const;
  void RelocateLibrary();
  // A download or install moved along: tile text and the top bar's count.
  void DownloadChanged(const QString& key);
  // Back to the grid with this game selected; its settings if filtered out.
  void ShowGame(const std::string& id);
  // "Installing… 1.2 GB" for a game mid-install, else empty.
  QString InstallText(const std::string& id) const;  // `announce` is false for the bulk path, where one toast covers the batch
  // and per-game messages would be one notification per game.
  void RefreshMetadata(const std::string& id, bool announce = true);
  // Swaps the game card's fields for its art picker, and back.
  void OpenArtPicker();
  void CloseArtPicker(bool applied = false);
  bool ArtPickerOpen() const;
  void FetchMissingArtwork();
  void SyncDesktopEntries();
  void RemoveAllDesktopEntries();
  void ShowSteamGridDbNotice(bool asked_for, const mira_gui::ApiError& error);
  // Routes ui/ErrorHelp's fix-it buttons to this window's pages.
  void InstallErrorNavigator();
  void UpdateTileCover(const QString& id);

  // `live` is false for mirad's replayed history: applied, never announced.
  void HandleGameEvent(const std::string& type, const std::string& data, bool live);
  int FilterRow(const QString& key) const;

  QWidget* top_bar_ = nullptr;
  QLineEdit* search_ = nullptr;
  // One row per kFilters entry, each carrying its key in Qt::UserRole and a
  // live count via a custom row widget (see UpdateFilterCounts), which lives
  // inside filter_sort_popover_, not directly in the sidebar layout.
  QListWidget* filters_ = nullptr;
  // The sidebar's always-visible filter+sort pill; concrete type (a small
  // QWidget subclass with a plain on_clicked callback, matching LibraryGrid's
  // own pattern) is local to LibraryWindow.cpp.
  QWidget* filter_sort_button_ = nullptr;
  QLabel* filter_icon_ = nullptr;
  QLabel* filter_summary_label_ = nullptr;
  QLabel* sort_icon_ = nullptr;
  QLabel* sort_summary_label_ = nullptr;
  QLabel* filter_sort_chevron_ = nullptr;
  QWidget* filter_sort_popover_ = nullptr;
  // One button per mira_gui::SortOptions() entry, exclusive selection,
  // replaces the old QComboBox with a vertical list of full-width rows.
  QList<QPushButton*> sort_buttons_;
  LibraryGrid* grid_ = nullptr;
  QVBoxLayout* grid_layout_ = nullptr;
  mira_gui::GameTileDelegate* delegate_ = nullptr;
  QSlider* zoom_ = nullptr;
  QToolButton* sort_direction_ = nullptr;
  QToolButton* add_games_ = nullptr;
  // Sidebar nav row, styled like library_nav_. Settings' own back button
  // lives on the settings page itself (BuildSettingsPage), rebuilt fresh
  // alongside settings_panel_ on each open.
  QPushButton* settings_button_ = nullptr;
  // Set by "Save and leave", so the save that follows closes Settings.
  bool close_settings_after_save_ = false;
  // Moved here from the sidebar's old hamburger menu; see BuildTopBar.
  QToolButton* downloads_button_ = nullptr;
  QToolButton* refresh_button_ = nullptr;
  QToolButton* shortcuts_button_ = nullptr;
  QToolButton* about_button_ = nullptr;
  QWidget* top_bar_divider_ = nullptr;
  QToolButton* minimize_button_ = nullptr;
  QToolButton* maximize_button_ = nullptr;
  QToolButton* close_button_ = nullptr;

  // Library is checked/highlighted whenever content_stack_ shows splitter_
  // (see UpdateLibraryNavActive).
  QPushButton* library_nav_ = nullptr;
  QPushButton* runners_nav_ = nullptr;
  QToolButton* manage_sources_button_ = nullptr;
  QToolButton* fetch_art_button_ = nullptr;
  // One sidebar row per mira_gui::AllSources() entry, same order; only set up
  // sources that aren't hidden are visible.
  QList<QPushButton*> source_navs_;
  QList<QLabel*> source_counts_;
  QSet<QString> hidden_sources_;    // unticked "In sidebar"
  QSet<QString> disabled_sources_;  // <id>.enabled = false
  std::vector<QString> source_order_;  // saved order; see SourceOrder()
  QHash<QString, QString> source_account_;      // signed-in account, where a store says
  QHash<QString, qint64> source_imported_at_;   // last import, unix seconds
  QWidget* source_nav_container_ = nullptr;  // accepts source row drops
  QWidget* source_drop_line_ = nullptr;
  QPushButton* source_drag_row_ = nullptr;
  QPoint source_drag_start_;
  QWidget* pinned_heading_ = nullptr;
  QVBoxLayout* pinned_layout_ = nullptr;
  QString pinned_signature_;  // what the rows show now; see FillSidebarSection
  QWidget* recent_heading_ = nullptr;
  QString recent_signature_;
  QVBoxLayout* recent_layout_ = nullptr;
  int recent_count_ = 0;  // besides running games
  QToolButton* pinned_customize_ = nullptr;
  QToolButton* recent_customize_ = nullptr;
  mira_gui::sidebar::Style pinned_style_ = mira_gui::sidebar::Style::Covers;
  mira_gui::sidebar::Style recent_style_ = mira_gui::sidebar::Style::Covers;
  bool recent_when_ = true;
  QWidget* sidebar_card_overlay_ = nullptr;
  QGridLayout* sidebar_card_layout_ = nullptr;
  QWidget* sidebar_card_ = nullptr;
  // Installer prompts waiting for Settings, a game's card or another card to close.
  std::deque<std::pair<std::string, std::function<void()>>> pending_cards_;  // key, show
  bool show_source_counts_ = true;
  bool source_icons_ = true;
  QElapsedTimer last_row_click_;
  mira_gui::TabRow* library_tabs_ = nullptr;
  mira_gui::ContinueRow* continue_row_ = nullptr;
  bool continue_row_enabled_ = true;
  int continue_count_ = 3;
  bool source_page_tabs_ = true;
  bool drag_select_ = true;
  // Source pages' own tile widths, unless tile_size_synced_.
  std::map<std::string, int> source_tile_widths_;
  bool tile_size_synced_ = false;
  QVBoxLayout* source_nav_layout_ = nullptr;
  // Store signed in / launcher installed, by source id, as last asked.
  QHash<QString, bool> source_ready_;
  mira_gui::SourcePage* source_page_ = nullptr;
  mira_gui::RunnersPage* runners_page_ = nullptr;

  QSplitter* splitter_ = nullptr;
  // The splitter's right side: grid_page_, source_page_, or runners_page_.
  QStackedWidget* main_stack_ = nullptr;
  QWidget* grid_page_ = nullptr;
  // Swaps the splitter out for Settings, full-screen. A
  // game's edit card is a separate overlay (game_edit_overlay_) that stays
  // over the grid instead.
  QStackedWidget* content_stack_ = nullptr;
  // Rebuilt on every OpenSettings() so it starts synced to what's actually
  // saved, not stale edits left over from a discarded previous open.
  QWidget* settings_page_ = nullptr;
  mira_gui::SettingsPanel* settings_panel_ = nullptr;
  // A game's editable form, in a centered overlay card; see OpenGameDialog.
  // The overlay is a chrome sibling,
  // not a content_stack_ page, so the grid stays visible (dimmed) underneath.
  QWidget* game_edit_overlay_ = nullptr;
  QGridLayout* game_edit_overlay_layout_ = nullptr;
  QWidget* game_edit_card_ = nullptr;
  // Owns chrome (top_bar_ + content_stack_) at index 0 and game_edit_overlay_
  // at index 1 -- StackAll shows both always; this just decides which one is
  // raised on top, toggled in OpenGameDialog/CloseGameEdit.
  QStackedLayout* root_stack_ = nullptr;
  mira_gui::GameEditForm* game_edit_form_ = nullptr;
  mira_gui::HeroBackdrop* game_edit_backdrop_ = nullptr;  // the card itself
  mira_gui::CoverChip* game_edit_cover_ = nullptr;
  // The form, and the art picker once first opened.
  QStackedWidget* game_edit_stack_ = nullptr;
  mira_gui::ArtPickerPanel* game_edit_picker_ = nullptr;
  QLabel* game_edit_title_ = nullptr;
  QPushButton* game_edit_art_button_ = nullptr;
  QPushButton* game_edit_play_ = nullptr;
  // The form's unsaved changes, or the picker's pick while it's open.
  mira_gui::ChangeBar* game_edit_bar_ = nullptr;
  bool close_game_edit_after_save_ = false;
  QLabel* footer_ = nullptr;
  QLabel* empty_hint_ = nullptr;
  mira_gui::HoverCard* hover_card_ = nullptr;
  // Dwell before a recently played row's hover card.
  QTimer* recent_hover_ = nullptr;
  QPointer<QWidget> recent_hover_row_;

  mira_gui::GameLibraryModel* library_ = nullptr;
  mira_gui::GameFilterProxy* grid_games_ = nullptr;
  mira_gui::DownloadTracker* downloads_ = nullptr;
  mira_gui::DaemonSupervisor* daemon_supervisor_ = nullptr;  // "Start mirad" from a failure
  bool mirad_reachable_ = true;  // as of the last request or event connection, for the footer
  bool stream_dropped_ = false;  // the event stream lost mirad; its return resyncs the list
  // Bumped per RefreshGames, so an older reply landing late can't undo a newer one.
  int games_request_ = 0;
  QTimer* save_prefs_timer_ = nullptr;  // see ScheduleSavePrefs
  mira_gui::DownloadsPanel* downloads_panel_ = nullptr;
  // game.added events asking for their settings to open, gathered briefly
  // so a scan's burst of them opens nothing.
  std::vector<std::string> pending_added_;
  QTimer* added_timer_ = nullptr;
  // Games the user explicitly asked to refresh: a metadata failure for one
  // of these is worth a toast; the dozens from an automatic scan are not.
  std::set<std::string> awaiting_metadata_;
  bool steamgriddb_notice_shown_ = false;
  // Set by "Fetch missing cover art": its fetches were asked for, so a
  // missing SteamGridDB key is worth reporting.
  bool artwork_fetch_requested_ = false;
  // The tile width Ctrl+0 returns to, and the one a frontend.toml with
  // no tile_width starts at.
  static constexpr int kDefaultTileWidth = 168;
  static constexpr int kMinTileWidth = 120;
  static constexpr int kMaxTileWidth = 260;
  int tile_width_ = kDefaultTileWidth;
  static constexpr int kDefaultSourceTileWidth = 150;  // smaller: a source page holds two grids
  std::string sort_key_ = "name";
  bool sort_descending_ = false;
  bool scan_on_startup_ = true;
  // Keyed by "<id>@<tile width>". A generated cover is cheap but not free,
  // and ApplyFilter() rebuilds every visible tile on each keystroke.
  mira_gui::ArtworkStore* artwork_ = nullptr;
  mira_gui::shortcuts::Common common_;
};
