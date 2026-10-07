#pragma once

#include <QElapsedTimer>
#include <QMainWindow>
#include <QString>

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../app/Shortcuts.h"
#include "../client/Types.h"

class QGridLayout;
class QSlider;
class QSplitter;
class QStackedLayout;
class QStackedWidget;
class QTimer;

namespace mira_gui {
class ArtworkStore;
class DaemonSupervisor;
class DownloadTracker;
class DownloadsPanel;
class GameCard;
class GameLibraryModel;
class GameMenus;
class HoverCard;
class LibraryPage;
class OwnedTitles;
class RunnersPage;
class TagsPage;
class SettingsPanel;
class Sidebar;
class SourcePage;
class TopBar;
struct SourceInfo;
}  // namespace mira_gui

// The main window: the top bar over the sidebar and, beside it, the library
// page, a source page or Runners; Settings full-screen in their place; and a
// game's card or a sidebar card in an overlay above. Frameless, so it owns
// its own move/resize/minimize/maximize/close. This class puts the pieces
// together: it decides what's on screen and routes each piece's requests.
class LibraryWindow : public QMainWindow {
  Q_OBJECT

public:
  // `prefs` is frontend.toml as read at startup; the theme is already applied.
  explicit LibraryWindow(const mira_gui::FrontendPrefs& prefs, QWidget* parent = nullptr);

private:
  mira_gui::Sidebar* BuildSidebar(const mira_gui::FrontendPrefs& prefs);
  mira_gui::LibraryPage* BuildLibraryPage(const mira_gui::FrontendPrefs& prefs, int tile_width);
  QWidget* BuildSettingsPage();
  void BuildShortcuts();

  // Frontend's own state (size, tile size, which filter) round-trips through
  // frontend.toml, not settings.toml. The window's own layout is read in the
  // constructor; these are what the settings screen also changes.
  void ApplySettingsPrefs(const mira_gui::FrontendPrefs& prefs);
  // A config.changed event's prefs, made by another client (the CLI, another window).
  void ApplyChangedPrefs(const std::string& payload);
  // The layout this window owns (size, zoom, filter, sort, sidebar), saved
  // in the background a moment after it last changed, so a crash loses at
  // most that moment. FlushPrefs writes a pending one now: waiting for it
  // when quitting, in the background when only hiding to the tray.
  mira_gui::FrontendPrefs LayoutPrefs() const;
  void ScheduleSavePrefs();
  void FlushPrefs(bool quitting = true);
  void resizeEvent(QResizeEvent* event) override;
  void closeEvent(QCloseEvent* event) override;
  void QuitOrClose();
  void changeEvent(QEvent* event) override;

  // Lists the library and scans it at once; the scan's changes arrive as
  // events. `force_scan` is Refresh's: startup honours scan_on_startup.
  void Reload(bool force_scan);
  void RefreshGames();
  void ConnectionChanged(bool connected);

  // After any change to library_: the footer, an open card, Runners.
  void LibraryChanged();
  void UpdateFooter();
  void UpsertGames(const std::vector<mira_gui::GameSummary>& games);
  void RemoveGame(const std::string& id);
  const mira_gui::GameSummary* FindGame(const std::string& id) const;

  // The slider moved: resizes whichever page is showing.
  void Zoom(int width);
  int SourceTileWidth(const QString& id) const;
  // Points the slider at the page on screen, and off where there's no grid.
  void SyncZoom();
  bool SourcePageShown() const;
  // The Tags page's game picker, whose covers follow the tile size.
  bool TagPickerShown() const;
  bool TagsShown() const;

  // One persistent HoverCard for every tile and sidebar row. `anchor` is
  // global; the card goes beside it.
  void ShowHoverCardFor(const mira_gui::GameSummary& game, const QRect& anchor,
                        const QString& hint = QString());
  void HideHoverCard();
  void ToggleRunning(const std::string& id);
  // A double-click that didn't play anything says why, on that game's tile, for a few seconds.
  void ExplainDoubleClickOff(const std::string& id);
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
  void SizeGameEditCard(QWidget* card);
  void CloseGameEdit();
  // Confirms first if the card has unsaved edits; the sidebar's Library nav
  // row, and a click on the scrim.
  void RequestCloseGameEdit();
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
  // Points the sidebar's highlight and the slider at what's on screen.
  void UpdateLibraryNavActive();
  void ShowLibrary();
  void OpenManageSources();
  // A card that changes the sidebar (the pinned and recently played style,
  // Manage sources), over the content with the sidebar left undimmed as its
  // preview. Showing one replaces any other.
  QWidget* BuildSidebarCardOverlay();
  void ShowSidebarCard(QWidget* card);
  void CloseSidebarCard();
  bool SidebarCardOpen() const;
  void OpenSidebarStyle();
  // A sidebar row or card's click. Ignores the second click of a double
  // click, which would otherwise land on whatever row moved under it.
  void RowClicked(const std::string& id);
  // Closes Settings and a game's card, asking first if either has unsaved
  // edits. False while one stays open.
  bool LeaveOverlays();
  // The Runners page, in the grid's place like a source page.
  void OpenRunners();
  void CloseRunners();
  // The Tags page is made once, ahead of its first open, and kept.
  void BuildTagsPage();
  void OpenTags();
  void CloseTags();
  void OpenAbout();
  void OpenGameDetailPage(const std::string& id);
  // A store or launcher's page, rebuilt fresh on each open.
  void OpenSource(const mira_gui::SourceInfo& source);
  // False while the page stays: its settings card's edits were kept, or are
  // saving first, and then `retry` runs (CloseSource itself when empty).
  bool CloseSource(std::function<void()> retry = {});
  // Asks about the open source page's unsaved settings; CloseSource's rules.
  bool ConfirmLeaveSource(std::function<void()> retry);
  void SetSourceControlsEnabled(bool enabled);
  // The grid is what's on screen: not Settings, Runners, or a source page.
  bool GridShown() const;
  // A download or install moved along: tile text and the top bar's count.
  void DownloadChanged(const QString& key);
  // Back to the grid with this game selected; its settings if filtered out.
  void ShowGame(const std::string& id);
  // "Installing… 1.2 GB" for a game mid-install, else empty.
  QString InstallText(const std::string& id) const;
  // `announce` is false for the bulk path, where one toast covers the batch
  // and per-game messages would be one notification per game.
  void RefreshMetadata(const std::string& id, bool announce = true);
  void FetchMissingArtwork();
  void ShowSteamGridDbNotice(bool asked_for, const mira_gui::ApiError& error);
  // Routes app/ErrorHelp's fix-it buttons to this window's pages.
  void InstallErrorNavigator();
  void UpdateTileCover(const QString& id);

  // `live` is false for mirad's replayed history: applied, never announced.
  void HandleGameEvent(const std::string& type, const std::string& data, bool live);

  mira_gui::TopBar* top_bar_ = nullptr;
  QSlider* zoom_ = nullptr;
  mira_gui::Sidebar* sidebar_ = nullptr;
  // Set by "Save and leave", so the save that follows closes Settings.
  bool close_settings_after_save_ = false;
  QWidget* sidebar_card_overlay_ = nullptr;
  QGridLayout* sidebar_card_layout_ = nullptr;
  QWidget* sidebar_card_ = nullptr;
  // Installer prompts waiting for Settings, a game's card or another card to close.
  std::deque<std::pair<std::string, std::function<void()>>> pending_cards_;  // key, show
  QElapsedTimer last_row_click_;
  bool source_page_tabs_ = true;
  bool drag_select_ = true;
  bool double_click_play_ = true;
  // Source pages' own tile widths, unless tile_size_synced_.
  std::map<std::string, int> source_tile_widths_;
  bool tile_size_synced_ = false;
  mira_gui::SourcePage* source_page_ = nullptr;
  mira_gui::RunnersPage* runners_page_ = nullptr;
  mira_gui::TagsPage* tags_page_ = nullptr;

  QSplitter* splitter_ = nullptr;
  // The splitter's right side: grid_page_, source_page_, or runners_page_.
  QStackedWidget* main_stack_ = nullptr;
  mira_gui::LibraryPage* grid_page_ = nullptr;
  // Swaps the splitter out for Settings, full-screen. A game's card is a
  // separate overlay (game_edit_overlay_) that stays over the grid instead.
  QStackedWidget* content_stack_ = nullptr;
  // Rebuilt on every OpenSettings() so it starts synced to what's actually
  // saved, not stale edits left over from a discarded previous open.
  QWidget* settings_page_ = nullptr;
  bool settings_loading_ = false;  // built, and shown once its panel is Ready
  mira_gui::SettingsPanel* settings_panel_ = nullptr;
  // A chrome sibling, not a content_stack_ page, so the grid stays visible
  // (dimmed) underneath the game's card.
  QWidget* game_edit_overlay_ = nullptr;
  QGridLayout* game_edit_overlay_layout_ = nullptr;
  mira_gui::GameCard* game_card_ = nullptr;
  // Owns chrome (top_bar_ + content_stack_) at index 0 and the overlays
  // after it. StackAll shows them all; this decides which one is raised.
  QStackedLayout* root_stack_ = nullptr;
  mira_gui::HoverCard* hover_card_ = nullptr;

  mira_gui::GameLibraryModel* library_ = nullptr;
  mira_gui::GameMenus* menus_ = nullptr;
  mira_gui::DownloadTracker* downloads_ = nullptr;
  mira_gui::OwnedTitles* owned_titles_ = nullptr;  // what a library search finds in the stores
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
  static constexpr int kDefaultSourceTileWidth = 150;  // smaller: a source page holds two grids
  bool scan_on_startup_ = true;
  std::string applied_prefs_;  // the frontend table (minus window layout) last applied from config.changed
  // Keyed by "<id>@<tile width>". A generated cover is cheap but not free,
  // and a filter or search change redraws every visible tile.
  mira_gui::ArtworkStore* artwork_ = nullptr;
  mira_gui::shortcuts::Common common_;
};
