#include "LibraryWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QScreen>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "../activity/DownloadTracker.h"
#include "../activity/DownloadsPanel.h"
#include "../dialogs/LogWindow.h"
#include "../app/DaemonSupervisor.h"
#include "../app/KeyBindings.h"
#include "../app/Notify.h"
#include "../app/Tray.h"
#include "../client/EventHub.h"
#include "../client/Events.h"
#include "../client/api/Artwork.h"
#include "../client/api/Config.h"
#include "../client/api/Games.h"
#include "../client/api/Library.h"
#include "../client/api/Stores.h"
#include "../dialogs/GameDetailPageDialog.h"
#include "../game/GameCard.h"
#include "../game/InstallPromptCard.h"
#include "../game/InstallerCards.h"
#include "../library/ArtworkStore.h"
#include "../library/GameActions.h"
#include "../library/GameLibraryModel.h"
#include "../library/GameMenus.h"
#include "../library/HoverCard.h"
#include "../library/LibraryActions.h"
#include "../library/OwnedTitles.h"
#include "../library/LibraryPage.h"
#include "../runners/RunnersPage.h"
#include "../settings/SettingsPanel.h"
#include "../sidebar/Sidebar.h"
#include "../sidebar/SidebarStyleCard.h"
#include "../sources/ManageSourcesCard.h"
#include "../sources/SourcePage.h"
#include "../sources/SourceSettingsCard.h"
#include "../sources/Sources.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/ModalOverlay.h"
#include "AboutPanel.h"
#include "FramelessRoot.h"
#include "TopBar.h"

LibraryWindow::LibraryWindow(const mira_gui::FrontendPrefs& prefs, QWidget* parent) : QMainWindow(parent) {
  setWindowTitle("Mira");
  // Custom top bar takes over move/resize/minimize/maximize/close, so no OS
  // decoration left.
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
  // Sized and centered before the first show: resizing once shown grows the
  // window from its top-left corner, off center.
  QSize size(1180, 720);
  if (prefs.window_width && prefs.window_height) size = QSize(*prefs.window_width, *prefs.window_height);
  if (const QScreen* screen = QGuiApplication::primaryScreen()) {
    const QRect available = screen->availableGeometry();
    size = size.boundedTo(available.size());
    setGeometry(QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, size, available));
  } else {
    resize(size);
  }
  if (prefs.window_maximized.value_or(false)) setWindowState(windowState() | Qt::WindowMaximized);

  // Before the widgets that show them are built, so they start right.
  const int tile_width =
      prefs.tile_width ? std::clamp(*prefs.tile_width, kMinTileWidth, kMaxTileWidth) : kDefaultTileWidth;
  for (const auto& [id, width] : prefs.source_tile_widths.value_or(std::map<std::string, int>{})) {
    source_tile_widths_[id] = std::clamp(width, kMinTileWidth, kMaxTileWidth);
  }

  // The one copy of the library every view reads; see GameLibraryModel.
  library_ = new mira_gui::GameLibraryModel(this);
  library_->install_progress = [this](const std::string& id) -> std::optional<mira_gui::DownloadTracker::TileProgress> {
    const mira_gui::DownloadTracker::Entry* entry = downloads_->Find(mira_gui::DownloadTracker::KeyFor(
        mira_gui::DownloadTracker::Kind::Game, QString(), QString::fromStdString(id)));
    if (entry == nullptr || entry->state != mira_gui::DownloadTracker::State::Running) return std::nullopt;
    return mira_gui::DownloadTracker::TileProgressFor(*entry);
  };
  connect(library_, &mira_gui::GameLibraryModel::Changed, this, &LibraryWindow::LibraryChanged);
  menus_ = new mira_gui::GameMenus(
      this, {.find = [this](const std::string& id) { return FindGame(id); },
             .install_text = [this](const std::string& id) { return InstallText(id); },
             .toggle_running = [this](const std::string& id) { ToggleRunning(id); },
             .open_settings = [this](const std::string& id) { OpenGameDialog(id); },
             .open_details = [this](const std::string& id) { OpenGameDetailPage(id); },
             .offer_install = [this](const std::string& id) { OfferInstall(id); },
             .refresh_metadata = [this](const std::string& id) { RefreshMetadata(id); },
             .removed = [this](const std::string& id) { RemoveGame(id); },
             .upsert = [this](const std::vector<mira_gui::GameSummary>& games) { UpsertGames(games); }});

  // Before the panel and the grid, because both ask it for covers.
  artwork_ = new mira_gui::ArtworkStore(this);
  connect(artwork_, &mira_gui::ArtworkStore::CoverChanged, this, &LibraryWindow::UpdateTileCover);

  InstallErrorNavigator();

  // Before the top bar, which shows its count.
  downloads_ = new mira_gui::DownloadTracker(this);
  owned_titles_ = new mira_gui::OwnedTitles(this);
  downloads_->game_name = [this](const std::string& id) {
    const mira_gui::GameSummary* game = FindGame(id);
    return game != nullptr ? QString::fromStdString(game->name) : QString();
  };
  downloads_->source_name = [](const QString& id) {
    const mira_gui::SourceInfo* source = mira_gui::FindSourceInfo(id);
    return source != nullptr ? source->name : id;
  };
  connect(downloads_, &mira_gui::DownloadTracker::Changed, this, &LibraryWindow::DownloadChanged);
  downloads_panel_ = new mira_gui::DownloadsPanel(downloads_, artwork_, this);
  connect(downloads_panel_, &mira_gui::DownloadsPanel::LogRequested, this,
          [this](const QString& channel, const QString& title) {
            downloads_panel_->hide();
            mira_gui::LogWindow::Open(this, channel, title);
          });
  connect(downloads_panel_, &mira_gui::DownloadsPanel::ShowGameRequested, this,
          [this](const QString& id) { ShowGame(id.toStdString()); });

  // The stylesheet re-polishes every widget by itself; what it cannot reach
  // is what we paint: the placeholder covers drawn in the theme's own colors.
  // Connected before the views are built, so it runs before they repaint.
  connect(mira_gui::theme::Notifier::Instance(), &mira_gui::theme::Notifier::Changed, this,
          [this] { artwork_->InvalidateAllRenderings(); });

  splitter_ = new QSplitter(Qt::Horizontal, this);
  sidebar_ = BuildSidebar(prefs);
  splitter_->addWidget(sidebar_);
  // A source page or Runners takes the grid's place here, leaving the sidebar up.
  main_stack_ = new QStackedWidget(this);
  grid_page_ = BuildLibraryPage(prefs, tile_width);
  main_stack_->addWidget(grid_page_);
  splitter_->addWidget(main_stack_);
  splitter_->setStretchFactor(0, 0);
  splitter_->setStretchFactor(1, 1);
  const int sidebar = prefs.sidebar_width.value_or(232);
  splitter_->setSizes({sidebar, std::max(400, width() - sidebar)});
  splitter_->setChildrenCollapsible(false);
  connect(splitter_, &QSplitter::splitterMoved, this, &LibraryWindow::ScheduleSavePrefs);

  // Settings is built lazily by OpenSettings() and covers this slot. A
  // game's edit card is a separate overlay, not a page here.
  content_stack_ = new QStackedWidget(this);
  content_stack_->addWidget(splitter_);

  auto* central = new mira_gui::FramelessRoot(this);
  // StackAll: the game-edit overlay is a chrome sibling, not a
  // content_stack_ page, so the grid/sidebar stay visible (dimmed)
  // underneath. current_widget only picks which one is raised.
  root_stack_ = new QStackedLayout(central);
  root_stack_->setStackingMode(QStackedLayout::StackAll);
  root_stack_->setContentsMargins(0, 0, 0, 0);

  auto* chrome = new QWidget(central);
  auto* layout = new QVBoxLayout(chrome);
  layout->setContentsMargins(mira_gui::kResizeMargin, mira_gui::kResizeMargin, mira_gui::kResizeMargin,
                             mira_gui::kResizeMargin);
  layout->setSpacing(0);
  top_bar_ = new mira_gui::TopBar(kMinTileWidth, kMaxTileWidth, tile_width, chrome);
  zoom_ = top_bar_->zoom();
  connect(zoom_, &QSlider::valueChanged, this, &LibraryWindow::Zoom);
  connect(top_bar_, &mira_gui::TopBar::ActivityClicked, this,
          [this] { downloads_panel_->ShowBelow(top_bar_->activity_button()); });
  connect(top_bar_, &mira_gui::TopBar::RefreshClicked, this, [this] { Reload(/*force_scan=*/true); });
  connect(top_bar_, &mira_gui::TopBar::ShortcutsClicked, this, [this] { common_.reference->trigger(); });
  connect(top_bar_, &mira_gui::TopBar::AboutClicked, this, &LibraryWindow::OpenAbout);
  // Without an explicit cursor here, a resize cursor FramelessRoot set at its
  // edge margin would keep showing over the whole window after the drag ends.
  top_bar_->setCursor(Qt::ArrowCursor);
  layout->addWidget(top_bar_);
  content_stack_->setCursor(Qt::ArrowCursor);
  layout->addWidget(content_stack_, /*stretch=*/1);
  root_stack_->addWidget(chrome);

  game_edit_overlay_ = BuildGameEditOverlay();
  root_stack_->addWidget(game_edit_overlay_);
  root_stack_->setCurrentWidget(chrome);

  setCentralWidget(central);

  BuildShortcuts();
  UpdateLibraryNavActive();

  // Not Hidden: opening on hidden games reads as the library being gone.
  if (prefs.library_filter && *prefs.library_filter != "hidden") {
    grid_page_->SetFilterKey(QString::fromStdString(*prefs.library_filter));
  }
  ApplySettingsPrefs(prefs);

  mira_gui::EventHub* hub = mira_gui::EventHub::Instance();
  connect(hub, &mira_gui::EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) { HandleGameEvent(type, data, live); });
  connect(hub, &mira_gui::EventHub::ConnectionChanged, this, &LibraryWindow::ConnectionChanged);
  hub->Start();

  qApp->installEventFilter(new mira_gui::FocusDropper(this));

  sidebar_->RefreshSources();
  Reload(/*force_scan=*/false);
}

void LibraryWindow::BuildShortcuts() {
  common_ = mira_gui::shortcuts::Install(
      this, {
                {"Ctrl+F", "Focus the search box"},
                {"Esc", "Clear the search, then the selection"},
                {"Ctrl+1…9", "Pick a filter"},
                {"Ctrl+H", "Toggle the Hidden filter"},
                {"F5, Ctrl+R", "Refresh the library"},
                {"Enter", "Play the selected game, or stop it while it runs"},
                {"Alt+Enter", "Game settings"},
                {"Delete", "Remove the selected games"},
                {"Ctrl++, Ctrl+-", "Tile size"},
                {"Ctrl+0", "Reset tile size"},
                {"Ctrl+,", "Settings"},
            });

  // Each of these registers with ui/KeyBindings so Settings' Shortcuts
  // category can list and edit it; Ctrl+1…9's per-filter loop below is the
  // one deliberate exception (see its own comment).
  auto window_action = [this](const QString& id, const QString& label, QKeySequence default_keys,
                              QList<QKeySequence> extra_aliases, auto slot) {
    auto* action = new QAction(this);
    const QKeySequence primary =
        mira_gui::keybindings::Register(action, id, label, default_keys, extra_aliases);
    QList<QKeySequence> keys{primary};
    keys.append(extra_aliases);
    action->setShortcuts(keys);
    connect(action, &QAction::triggered, this, slot);
    addAction(action);
  };

  // Scoped to the grid, not the window: Delete and Enter still have to mean
  // what they mean inside the search box. WidgetWithChildrenShortcut keeps a
  // keystroke aimed at a text field from reaching the library instead.
  auto grid_action = [this](const QString& id, const QString& label, QKeySequence default_keys,
                            QList<QKeySequence> extra_aliases, auto slot) {
    QWidget* scope = grid_page_->ShortcutScope();
    auto* action = new QAction(scope);
    const QKeySequence primary =
        mira_gui::keybindings::Register(action, id, label, default_keys, extra_aliases);
    QList<QKeySequence> keys{primary};
    keys.append(extra_aliases);
    action->setShortcuts(keys);
    action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(action, &QAction::triggered, this, slot);
    scope->addAction(action);
  };

  window_action("focus_search", "Focus the search box", QKeySequence(QKeySequence::Find), {},
                [this] { grid_page_->FocusSearch(); });

  // One key, three jobs, in the order a user expects to undo them: leave
  // settings first, then clear the search, then clear the selection.
  window_action("clear_or_deselect", "Clear the search, then the selection",
               QKeySequence(Qt::Key_Escape), {}, [this] {
    if (SidebarCardOpen()) {
      CloseSidebarCard();
      return;
    }
    if (source_page_ != nullptr && source_page_->SettingsModalOpen()) {
      source_page_->CloseSettingsModal();
      return;
    }
    if (SettingsOpen()) {
      RequestCloseSettings();
      return;
    }
    if (GameEditOpen()) {
      game_card_->Back();
      return;
    }
    grid_page_->ClearSearchOrSelection();
  });

  window_action("zoom_in", "Bigger tiles", QKeySequence(QKeySequence::ZoomIn),
               {QKeySequence(Qt::CTRL | Qt::Key_Equal)},
               [this] { zoom_->setValue(zoom_->value() + zoom_->pageStep()); });
  window_action("zoom_out", "Smaller tiles", QKeySequence(QKeySequence::ZoomOut), {},
               [this] { zoom_->setValue(zoom_->value() - zoom_->pageStep()); });
  window_action("reset_zoom", "Reset tile size", QKeySequence(Qt::CTRL | Qt::Key_0), {},
               [this] {
                 zoom_->setValue(SourcePageShown() && !tile_size_synced_ ? kDefaultSourceTileWidth : kDefaultTileWidth);
               });

  // Ctrl+1 through Ctrl+8, in filter order. Guarded by count() rather than
  // by the filter list so adding a ninth filter cannot walk past Ctrl+9. Not
  // registered with keybindings: nine near-identical rebindable rows for
  // "pick the Nth filter" isn't worth the Settings screen space, and the
  // filter list itself isn't fixed enough to make good default labels for.
  for (int row = 0; row < grid_page_->FilterCount() && row < 9; ++row) {
    auto* action = new QAction(this);
    action->setShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + row)));
    connect(action, &QAction::triggered, this, [this, row] { grid_page_->PickFilter(row); });
    addAction(action);
  }

  // A dedicated toggle for Hidden, on top of whatever Ctrl+9 already gives
  // it. Toggles back to All on a second press so it never strands the grid.
  window_action("toggle_hidden", "Toggle the Hidden filter", QKeySequence(Qt::CTRL | Qt::Key_H), {},
               [this] { grid_page_->ToggleHidden(); });

  // Neither is a menu entry anymore (both are sidebar rows now), kept here
  // so their shortcuts and Settings-screen Shortcuts-category listing
  // survive the menu trim.
  window_action("settings", "Settings", QKeySequence(Qt::CTRL | Qt::Key_Comma), {},
               [this] { OpenSettings(); });
  // F5 is the platform's own Refresh; Ctrl+R is the one every browser
  // taught, and a second binding costs nothing.
  window_action("refresh", "Refresh the library", QKeySequence(QKeySequence::Refresh),
               {QKeySequence(Qt::CTRL | Qt::Key_R)},
               [this] { Reload(/*force_scan=*/true); });

  // Qt::Key_Enter is the keypad one, a separate key from Qt::Key_Return,
  // and binding only Return would leave it dead.
  grid_action("play_stop", "Play the selected game, or stop it while it runs", QKeySequence(Qt::Key_Return),
             {QKeySequence(Qt::Key_Enter)}, [this] {
               const mira_gui::GameSummary* game = FindGame(grid_page_->SelectedId());
               if (game == nullptr || !mira_gui::CanPlayOrStop(*game)) return;
               ToggleRunning(std::string(game->id));
             });

  grid_action("details_settings", "Game settings", QKeySequence(Qt::ALT | Qt::Key_Return),
             {QKeySequence(Qt::ALT | Qt::Key_Enter)}, [this] {
               const std::string id = grid_page_->SelectedId();
               if (!id.empty()) OpenGameDialog(id);
             });

  grid_action("delete_game", "Remove the selected games", QKeySequence(Qt::Key_Delete), {}, [this] {
    // Copied before the call: the dialog's event loop can change the library.
    const std::vector<std::pair<std::string, QString>> selected = grid_page_->SelectedGames();
    if (selected.size() > 1) {
      mira_gui::actions::BatchDelete(this, selected, nullptr);
    } else if (selected.size() == 1) {
      const std::string id = selected.front().first;
      mira_gui::actions::Delete(this, id, selected.front().second, [this, id] { RemoveGame(id); });
    }
  });
}

void LibraryWindow::ApplySettingsPrefs(const mira_gui::FrontendPrefs& prefs) {
  scan_on_startup_ = prefs.scan_on_startup.value_or(true);
  sidebar_->ApplyPrefs(prefs);
  source_page_tabs_ = prefs.source_page_tabs.value_or(true);
  tile_size_synced_ = prefs.tile_size_synced.value_or(false);
  drag_select_ = prefs.drag_select.value_or(true);
  double_click_play_ = prefs.double_click_play.value_or(true);
  if (source_page_ != nullptr) source_page_->SetDragSelectEnabled(drag_select_);
  UpdateLibraryNavActive();  // the slider follows tile_size_synced_
  grid_page_->ApplyPrefs(prefs);
}

void LibraryWindow::ScheduleSavePrefs() {
  // Before it's shown, "changes" are just the saved layout being applied.
  if (!isVisible()) return;
  if (save_prefs_timer_ == nullptr) {
    save_prefs_timer_ = new QTimer(this);
    save_prefs_timer_->setSingleShot(true);
    // Long enough that dragging the zoom slider or the window edge saves once.
    save_prefs_timer_->setInterval(1000);
    connect(save_prefs_timer_, &QTimer::timeout, this, [this] {
      mira_gui::api::SaveFrontendPrefsAsync(this, LayoutPrefs(), [](mira_gui::PatchConfigResult) {});
    });
  }
  save_prefs_timer_->start();
}

void LibraryWindow::FlushPrefs() {
  if (save_prefs_timer_ == nullptr || !save_prefs_timer_->isActive()) return;
  save_prefs_timer_->stop();
  // Blocking: an async save's thread might not reach the socket before the
  // process exits. Failure isn't reported: the cost is a layout, not data.
  mira_gui::api::SaveFrontendPrefsBlocking(LayoutPrefs());
}

mira_gui::FrontendPrefs LibraryWindow::LayoutPrefs() const {
  mira_gui::FrontendPrefs prefs;
  // The unmaximized size, or a restart would open a normal window as big as the screen.
  const QSize normal = isMaximized() || isFullScreen() ? normalGeometry().size() : size();
  if (normal.isValid()) {
    prefs.window_width = normal.width();
    prefs.window_height = normal.height();
  }
  prefs.window_maximized = isMaximized();
  prefs.tile_width = grid_page_->TileWidth();
  prefs.source_tile_widths = source_tile_widths_;
  prefs.library_filter = grid_page_->FilterKey().toStdString();
  prefs.sort_by = grid_page_->SortKey();
  prefs.sort_descending = grid_page_->SortDescending();
  const QList<int> sizes = splitter_->sizes();
  if (sizes.size() == 2) prefs.sidebar_width = sizes[0];
  return prefs;
}

void LibraryWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  SizeGameEditCard(game_card_);
  ScheduleSavePrefs();
}

void LibraryWindow::changeEvent(QEvent* event) {
  if (event->type() == QEvent::WindowStateChange && top_bar_ != nullptr) {
    top_bar_->SyncMaximized();
    ScheduleSavePrefs();
  }
  QMainWindow::changeEvent(event);
}

void LibraryWindow::QuitOrClose() {
  if (mira_gui::tray::IsManaged(this)) {
    mira_gui::tray::RequestQuit();
  } else {
    close();
  }
}

void LibraryWindow::closeEvent(QCloseEvent* event) {
  // Only for the window Attach() made the tray's; a secondary window
  // closes for real either way, since nothing would bring it back.
  if (mira_gui::tray::IsManaged(this) && !mira_gui::tray::Quitting()) {
    FlushPrefs();
    event->ignore();
    hide();
    return;
  }

  if (!ConfirmLeaveSource([this] { close(); })) {
    event->ignore();
    return;
  }
  const bool settings_dirty = settings_panel_ != nullptr && settings_panel_->IsDirty();
  const bool game_dirty = game_card_ != nullptr && GameEditOpen() && game_card_->IsDirty();
  // Neither Save() finishes synchronously, so quit for real only once it
  // has, via the one-shot below, not this closeEvent call.
  const auto save = [this, settings_dirty] {
    if (settings_dirty) {
      close_settings_after_save_ = true;
      connect(settings_panel_, &mira_gui::SettingsPanel::SaveFinished, this,
              [this](bool ok, QString) {
                if (ok) QuitOrClose();
              },
              Qt::SingleShotConnection);
      settings_panel_->Save();
    } else {
      connect(game_card_, &mira_gui::GameCard::Saved, this, &LibraryWindow::QuitOrClose,
              Qt::SingleShotConnection);
      game_card_->Save();
    }
  };
  if ((settings_dirty || game_dirty) &&
      !mira_gui::notify::LeaveUnsaved(
          this, settings_dirty ? "Settings changed but not saved." : "This game's edits aren't saved.",
          save)) {
    event->ignore();
    return;
  }

  FlushPrefs();
  mira_gui::api::ClearArtThumbsBlocking();
  QMainWindow::closeEvent(event);
}

void LibraryWindow::OpenRunners() {
  if (!LeaveOverlays()) return;
  if (runners_page_ != nullptr) {
    UpdateLibraryNavActive();
    return;
  }
  if (source_page_ != nullptr && !CloseSource([this] { OpenRunners(); })) return;
  runners_page_ = new mira_gui::RunnersPage(downloads_, this);
  runners_page_->SetGames(library_->Games());
  main_stack_->addWidget(runners_page_);
  main_stack_->setCurrentWidget(runners_page_);
  SetSourceControlsEnabled(false);
  UpdateLibraryNavActive();
}

void LibraryWindow::CloseRunners() {
  if (runners_page_ == nullptr) return;
  main_stack_->setCurrentWidget(grid_page_);
  main_stack_->removeWidget(runners_page_);
  runners_page_->deleteLater();
  runners_page_ = nullptr;
  SetSourceControlsEnabled(true);
  UpdateLibraryNavActive();
}

void LibraryWindow::OpenAbout() {
  QDialog dialog(this);
  dialog.setWindowTitle("About Mira");
  auto* layout = new QVBoxLayout(&dialog);
  layout->addWidget(new mira_gui::AboutPanel(&dialog));
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
  auto* daemon_log = buttons->addButton("Service log", QDialogButtonBox::ActionRole);
  daemon_log->setToolTip("What Mira's background service is doing, live");
  connect(daemon_log, &QPushButton::clicked, this, [this] { mira_gui::LogWindow::Open(this, "daemon", "Mira service"); });
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  dialog.resize(560, dialog.sizeHint().height());
  dialog.exec();
}

void LibraryWindow::OpenGameDetailPage(const std::string& id) {
  const mira_gui::GameSummary* game = FindGame(id);
  const QString name = game != nullptr ? QString::fromStdString(game->name) : QString();
  mira_gui::GameDetailPageDialog dialog(id, name, this);
  dialog.exec();
}

mira_gui::Sidebar* LibraryWindow::BuildSidebar(const mira_gui::FrontendPrefs& prefs) {
  using mira_gui::Sidebar;
  auto* sidebar = new Sidebar(library_, artwork_, prefs, this);
  connect(sidebar, &Sidebar::LibraryClicked, this, &LibraryWindow::ShowLibrary);
  connect(sidebar, &Sidebar::RunnersClicked, this, &LibraryWindow::OpenRunners);
  connect(sidebar, &Sidebar::SettingsRequested, this, &LibraryWindow::OpenSettings);
  connect(sidebar, &Sidebar::SourceClicked, this, &LibraryWindow::OpenSource);
  connect(sidebar, &Sidebar::ManageSourcesRequested, this, &LibraryWindow::OpenManageSources);
  connect(sidebar, &Sidebar::StyleRequested, this, &LibraryWindow::OpenSidebarStyle);
  connect(sidebar, &Sidebar::FetchArtRequested, this, &LibraryWindow::FetchMissingArtwork);
  connect(sidebar, &Sidebar::PlayRequested, this, &LibraryWindow::RowClicked);
  connect(sidebar, &Sidebar::GameMenuRequested, this, [this](const std::string& id, const QPoint& pos) {
    // A row that's part of a larger selection acts for all of it, as a tile does.
    const auto selected = grid_page_->SelectedGames();
    if (selected.size() > 1 && std::ranges::contains(selected, id, &std::pair<std::string, QString>::first)) {
      std::vector<std::string> ids;
      for (const auto& [game_id, name] : selected) ids.push_back(game_id);
      menus_->ShowBatchMenu(ids, pos);
      return;
    }
    menus_->ShowGameMenu(id, pos);
  });
  connect(sidebar, &Sidebar::SelectionToggled, this,
          [this](const std::string& id) { grid_page_->ToggleSelected(id); });
  connect(sidebar, &Sidebar::HoverRequested, this,
          [this](const std::string& id, const QRect& anchor, const QString& hint) {
            if (const mira_gui::GameSummary* game = FindGame(id)) ShowHoverCardFor(*game, anchor, hint);
          });
  connect(sidebar, &Sidebar::HoverEnded, this, &LibraryWindow::HideHoverCard);
  connect(sidebar, &Sidebar::SourcesChanged, this, [this] {
    if (auto* card = qobject_cast<mira_gui::ManageSourcesCard*>(sidebar_card_)) {
      card->SetEntries(sidebar_->SourceEntries());
    }
  });
  return sidebar;
}

mira_gui::LibraryPage* LibraryWindow::BuildLibraryPage(const mira_gui::FrontendPrefs& prefs,
                                                       int tile_width) {
  auto* page = new mira_gui::LibraryPage(library_, artwork_, prefs.sort_by.value_or("name"),
                                         prefs.sort_descending.value_or(false), tile_width, this);
  connect(page, &mira_gui::LibraryPage::FilterChanged, this, &LibraryWindow::ScheduleSavePrefs);
  connect(page, &mira_gui::LibraryPage::SortChanged, this, &LibraryWindow::ScheduleSavePrefs);
  connect(page, &mira_gui::LibraryPage::ShownChanged, this, [this] {
    UpdateFooter();
    sidebar_->SetShowingHidden(grid_page_->FilterKey() == "hidden");  // pinned rows follow it
  });
  connect(page, &mira_gui::LibraryPage::GameActivated, this, [this](const std::string& id) {
    const mira_gui::GameSummary* game = FindGame(id);
    if (game == nullptr) return;
    if (!double_click_play_) {
      ExplainDoubleClickOff(id);
      return;
    }
    // A game that still needs installing installs instead.
    if (game->status == "needs_install" && InstallText(id).isEmpty()) {
      OfferInstall(id);
      return;
    }
    if (mira_gui::CanPlayOrStop(*game)) ToggleRunning(id);
  });
  connect(page, &mira_gui::LibraryPage::PlayRequested, this, &LibraryWindow::RowClicked);
  page->SetOwnedTitles(owned_titles_);
  page->title_progress = [this](const QString& source,
                                const QString& ref) -> std::optional<mira_gui::DownloadTracker::TileProgress> {
    const mira_gui::DownloadTracker::Entry* entry =
        downloads_->Find(mira_gui::DownloadTracker::KeyFor(mira_gui::DownloadTracker::Kind::Title, source, ref));
    if (entry == nullptr || entry->state != mira_gui::DownloadTracker::State::Running) return std::nullopt;
    return mira_gui::DownloadTracker::TileProgressFor(*entry);
  };
  connect(page, &mira_gui::LibraryPage::InstallTitleRequested, this, [this](const QString& source, const QString& ref) {
    mira_gui::api::InstallStoreTitleAsync(this, source.toStdString(), ref.toStdString(), /*update=*/false,
                                          [this](mira_gui::StoreActionResult result) {
                                            // Progress and the outcome arrive as events.
                                            if (!result.ok) mira_gui::notify::FailedRequest(this, "Could not start the install.", result.error);
                                          });
  });
  connect(page, &mira_gui::LibraryPage::SelectionChanged, this, [this, page] {
    QSet<QString> ids;
    for (const auto& [id, name] : page->SelectedGames()) ids.insert(QString::fromStdString(id));
    if (sidebar_ != nullptr) sidebar_->SetSelectedGames(ids);
  });
  connect(page, &mira_gui::LibraryPage::GameMenuRequested, this,
          [this](const std::string& id, const QPoint& pos) { menus_->ShowGameMenu(id, pos); });
  connect(page, &mira_gui::LibraryPage::BatchMenuRequested, this,
          [this](const std::vector<std::string>& ids, const QPoint& pos) { menus_->ShowBatchMenu(ids, pos); });
  connect(page, &mira_gui::LibraryPage::HoverRequested, this, [this](const std::string& id, const QRect& anchor) {
    // Nothing to preview once the grid isn't on screen.
    const mira_gui::GameSummary* game = FindGame(id);
    if (GridShown() && game != nullptr) ShowHoverCardFor(*game, anchor);
  });
  connect(page, &mira_gui::LibraryPage::HoverEnded, this, &LibraryWindow::HideHoverCard);
  connect(page, &mira_gui::LibraryPage::ZoomStepped, this,
          [this](int steps) { zoom_->setValue(zoom_->value() + steps * zoom_->pageStep()); });
  return page;
}

void LibraryWindow::Zoom(int width) {
  ScheduleSavePrefs();
  if (!SourcePageShown() || tile_size_synced_) grid_page_->SetTileWidth(width);
  if (!SourcePageShown()) return;
  if (!tile_size_synced_) source_tile_widths_[source_page_->property("source_id").toString().toStdString()] = width;
  source_page_->SetTileWidth(width);
}

int LibraryWindow::SourceTileWidth(const QString& id) const {
  if (tile_size_synced_) return grid_page_->TileWidth();
  const auto it = source_tile_widths_.find(id.toStdString());
  return it == source_tile_widths_.end() ? kDefaultSourceTileWidth : it->second;
}

bool LibraryWindow::SourcePageShown() const {
  return source_page_ != nullptr && content_stack_->currentWidget() == splitter_ &&
         main_stack_->currentWidget() == source_page_;
}

void LibraryWindow::SyncZoom() {
  if (zoom_ == nullptr) return;
  const bool source = SourcePageShown();
  const QSignalBlocker block(zoom_);  // showing a page's size isn't changing it
  zoom_->setValue(source ? SourceTileWidth(source_page_->property("source_id").toString())
                         : grid_page_->TileWidth());
  zoom_->setEnabled(!GameEditOpen() && (source || GridShown()));
}

void LibraryWindow::UpdateTileCover(const QString& id) {
  if (source_page_ != nullptr) source_page_->UpdateCover(id);
  grid_page_->UpdateCover(id.toStdString());
  // Whether or not the game has a tile.
  if (game_card_ != nullptr) game_card_->UpdateCover(id.toStdString());
  // Every view of that game repaints its row.
  library_->Touch(id.toStdString());
}

void LibraryWindow::InstallErrorNavigator() {
  const auto find_source = [](const std::string& id) {
    return mira_gui::FindSourceInfo(QString::fromStdString(id));
  };
  QPointer<LibraryWindow> self(this);
  mira_gui::error_help::Navigator nav;
  nav.open_setting = [self](const QString& key) {
    if (self) self->OpenSettings(key);
  };
  nav.open_runners = [self] {
    if (self) self->OpenRunners();
  };
  nav.open_source = [self, find_source](const std::string& id) {
    if (const mira_gui::SourceInfo* source = find_source(id); self && source != nullptr) self->OpenSource(*source);
  };
  nav.open_game_settings = [self](const std::string& id) {
    if (self) self->OpenGameDialog(id);
  };
  nav.view_log = [self](const std::string& id) {
    if (!self) return;
    const mira_gui::GameSummary* game = self->FindGame(id);
    mira_gui::actions::ViewLog(self, id, game != nullptr ? QString::fromStdString(game->name) : QString());
  };
  nav.install_shown = [self](const std::string& id) {
    if (!self) return;
    mira_gui::api::InstallGameAsync(self, id, /*interactive=*/true, std::string(),
                                            [self](mira_gui::GameActionResult result) {
                                              if (self && !result.ok) {
                                                mira_gui::notify::FailedRequest(self, "Could not start the installer.",
                                                                                result.error);
                                              }
                                            });
  };
  nav.start_daemon = [self] {
    if (!self) return;
    // Kept, not deleted after Ready: its destructor stops a mirad it started.
    if (self->daemon_supervisor_ == nullptr) {
      self->daemon_supervisor_ = new mira_gui::DaemonSupervisor(self);
      connect(self->daemon_supervisor_, &mira_gui::DaemonSupervisor::Ready, self,
              [self] { self->Reload(/*force_scan=*/false); });
      connect(self->daemon_supervisor_, &mira_gui::DaemonSupervisor::Failed, self,
              [self](const QString& error) {
                mira_gui::notify::FailedWithHint(self, "Could not start mirad.", error,
                                                 "Mira looks for mirad next to itself, then on PATH.");
              });
    }
    self->daemon_supervisor_->EnsureRunning();
  };
  nav.source_name = [find_source](const std::string& id) {
    const mira_gui::SourceInfo* source = find_source(id);
    return source != nullptr ? source->name : QString();
  };
  mira_gui::error_help::SetNavigator(std::move(nav));
}

void LibraryWindow::ShowSteamGridDbNotice(bool asked_for, const mira_gui::ApiError& error) {
  // Only when the user actually asked for art: a background fetch after a
  // scan hitting this would otherwise nag on every launch. Once per session,
  // however many games report it. mirad's hint and fix say what to do.
  if (!asked_for || steamgriddb_notice_shown_) return;
  steamgriddb_notice_shown_ = true;
  mira_gui::notify::FailedRequest(this, "No cover art without a SteamGridDB API key.", error);
}

void LibraryWindow::FetchMissingArtwork() {
  // Asked for, so a missing SteamGridDB key is worth saying (ShowSteamGridDbNotice).
  artwork_fetch_requested_ = true;
  mira_gui::actions::FetchMissingArtwork(this, [this] { artwork_fetch_requested_ = false; });
}

void LibraryWindow::RefreshMetadata(const std::string& id, bool announce) {
  mira_gui::api::RefreshMetadataAsync(
      this, id, announce, [this, id, announce](mira_gui::MetadataRefreshResult result) {
        if (!result.ok) {
          if (announce) {
            mira_gui::notify::FailedRequest(this, "Could not refresh metadata.", result.error);
          }
          return;
        }
        // 202: fetch runs on the daemon, reports back as an event. Remembered
        // so ShowSteamGridDbNotice knows this game was asked about.
        awaiting_metadata_.insert(id);
      });
}

void LibraryWindow::Reload(bool force_scan) {
  RefreshGames();
  // mirad's own watcher keeps the library current while it runs, so
  // skipping the startup scan costs nothing except a library that
  // changed while mirad was stopped.
  if (force_scan || scan_on_startup_) {
    mira_gui::api::ScanLibraryAsync(this, [](mira_gui::ScanResult) {});
  }
}

void LibraryWindow::RefreshGames() {
  // Hidden games included, so Ctrl+H is a client-side filter switch, not a round trip.
  const int request = ++games_request_;
  mira_gui::api::ListAllGamesAsync(this, [this, request](mira_gui::GamesResult result) {
    if (request != games_request_) return;  // a newer refresh is on its way
    mirad_reachable_ = result.ok;
    if (!result.ok) {
      mira_gui::notify::FailedRequest(this, "Could not list games.", result.error);
      return;  // what's shown stays, rather than emptying the library
    }
    // Before the tiles paint, so a game without art is never asked for it.
    for (const mira_gui::GameSummary& game : result.games) artwork_->NoteArt(game.id, game.art);
    library_->Replace(result.games);
  });
}

void LibraryWindow::ConnectionChanged(bool connected) {
  if (!connected) {
    stream_dropped_ = true;
    mirad_reachable_ = false;
    UpdateFooter();
    return;
  }
  mirad_reachable_ = true;
  UpdateFooter();
  // Ready before the first search; after startup's own requests, since it asks every store.
  QTimer::singleShot(5000, owned_titles_, &mira_gui::OwnedTitles::RefreshIfStale);
  // mirad restarted or came back: what changed meanwhile may be past its replay.
  if (std::exchange(stream_dropped_, false)) {
    RefreshGames();
    sidebar_->RefreshSources();
    downloads_->RecheckJobs();
  }
}

void LibraryWindow::LibraryChanged() {
  // Its game was removed elsewhere (a source, the CLI): saving would only fail.
  if (game_card_ != nullptr && library_->Find(game_card_->id()) == nullptr) CloseGameEdit();
  UpdateFooter();
  if (runners_page_ != nullptr) runners_page_->SetGames(library_->Games());
}

void LibraryWindow::UpdateFooter() {
  sidebar_->SetFooter(grid_page_->ShownCount(), static_cast<int>(library_->Games().size()),
                      mirad_reachable_);
}

const mira_gui::GameSummary* LibraryWindow::FindGame(const std::string& id) const { return library_->Find(id); }

void LibraryWindow::UpsertGames(const std::vector<mira_gui::GameSummary>& games) {
  // A rename changes the placeholder's initials, so the rendered tile is
  // stale even though the fetched artwork behind it isn't.
  for (const mira_gui::GameSummary& game : games) {
    artwork_->InvalidateRendering(game.id);
    artwork_->NoteArt(game.id, game.art);
  }
  library_->Upsert(games);
}

void LibraryWindow::RemoveGame(const std::string& id) { library_->Remove({id}); }

void LibraryWindow::HideHoverCard() {
  if (hover_card_ != nullptr) hover_card_->hide();
}

void LibraryWindow::ShowHoverCardFor(const mira_gui::GameSummary& game, const QRect& anchor,
                                     const QString& hint) {
  if (hover_card_ == nullptr) hover_card_ = new mira_gui::HoverCard(this);
  hover_card_->ShowGame(game, game.running, hint);
  hover_card_->PopUpBeside(anchor);
}

void LibraryWindow::AskAboutInstall(const mira_gui::InstallDetectedEvent& event) {
  const mira_gui::GameSummary* game = FindGame(event.id);
  if (game == nullptr) return;
  // Out of sight, a notification asks; its button brings Mira up on the card.
  if ((!isVisible() || isMinimized()) &&
      mira_gui::notify::AskOutOfSight(this, QString::fromStdString(game->name) + " installed a program",
                                      "Use it instead of the installer?", "Review…",
                                      [this, event] { ShowInstallPrompt(event); })) {
    return;
  }
  ShowInstallPrompt(event);
}

void LibraryWindow::QueueCard(const std::string& key, std::function<void()> show) {
  // Never over Settings, a game's card or another card: it waits until they close.
  if (SettingsOpen() || GameEditOpen() || SidebarCardOpen()) {
    if (std::ranges::none_of(pending_cards_, [&](const auto& queued) { return queued.first == key; })) {
      pending_cards_.emplace_back(key, std::move(show));
    }
    return;
  }
  show();
}

void LibraryWindow::ShowNextCard() {
  // Later, so a card closing itself is fully gone first.
  QTimer::singleShot(0, this, [this] {
    if (pending_cards_.empty() || SettingsOpen() || GameEditOpen() || SidebarCardOpen()) return;
    const std::function<void()> show = std::move(pending_cards_.front().second);
    pending_cards_.pop_front();
    show();
  });
}

void LibraryWindow::OfferInstall(const std::string& id) {
  QueueCard("install:" + id, [this, id] {
    const mira_gui::GameSummary* game = FindGame(id);
    // Asked again later, it may have been installed or removed meanwhile.
    if (game == nullptr || (game->status != "needs_install" && game->status != "broken")) {
      ShowNextCard();
      return;
    }
    auto* card = new mira_gui::InstallerCard(*game, artwork_);
    connect(card, &mira_gui::InstallerCard::CloseRequested, this, &LibraryWindow::CloseSidebarCard);
    connect(card, &mira_gui::InstallerCard::Started, this, &LibraryWindow::CloseSidebarCard);
    ShowSidebarCard(card);
  });
}

void LibraryWindow::OfferInstallerDelete(const mira_gui::InstallerLeftoverEvent& event) {
  QueueCard("installer-leftover:" + event.id, [this, event] {
    const mira_gui::GameSummary* game = FindGame(event.id);
    if (game == nullptr) {
      ShowNextCard();
      return;
    }
    auto* card = new mira_gui::InstallerLeftoverCard(*game, event.installer_dir, event.bytes, artwork_);
    connect(card, &mira_gui::InstallerLeftoverCard::CloseRequested, this, &LibraryWindow::CloseSidebarCard);
    ShowSidebarCard(card);
  });
}

void LibraryWindow::ShowInstallPrompt(const mira_gui::InstallDetectedEvent& event) {
  QueueCard("detected:" + event.id, [this, event] {
    const mira_gui::GameSummary* game = FindGame(event.id);
    if (game == nullptr) {
      ShowNextCard();
      return;
    }
    auto* card = new mira_gui::InstallPromptCard(*game, event.install_path, event.exe_path, artwork_);
    connect(card, &mira_gui::InstallPromptCard::CloseRequested, this, &LibraryWindow::CloseSidebarCard);
    connect(card, &mira_gui::InstallPromptCard::Accepted, this,
            [this, id = event.id, install_path = event.install_path](const std::string& exe_path, bool is_app) {
              CloseSidebarCard();
              mira_gui::api::FinishInstallAsync(
                  this, id,
                  [this, id, is_app](mira_gui::FinishInstallResult result) {
                    if (!result.ok) {
                      mira_gui::notify::FailedRequest(this, "Could not switch to the installed program.",
                                                      result.error);
                      return;
                    }
                    if (is_app) menus_->SetTag({id}, mira_gui::tags::kApp, true);
                  },
                  install_path, exe_path);
            });
    ShowSidebarCard(card);
  });
}

void LibraryWindow::ExplainDoubleClickOff(const std::string& id) {
  // On the tile itself: a tooltip would close with the double-click's own release.
  const QString note = "Double-click to play is off";
  if (SourcePageShown()) {
    source_page_->ShowTileNote(QString::fromStdString(id), note);
  } else {
    grid_page_->ShowTileNote(id, note);
  }
}

void LibraryWindow::ToggleRunning(const std::string& id) {
  const mira_gui::GameSummary* game = FindGame(id);
  if (game != nullptr && game->running) {
    mira_gui::actions::Stop(this, id);
  } else {
    LaunchGame(id);
  }
}

void LibraryWindow::LaunchGame(const std::string& id) {
  mira_gui::actions::Launch(this, id, [this, id](bool tracked) {
    // Ahead of mirad's own game.state, which says the same. An untracked
    // (Steam) launch never gets one, so it isn't marked at all.
    if (tracked) library_->SetRunning(id, true);
  });
}

void LibraryWindow::OpenGameDialog(const std::string& id) {
  if (!LeaveOverlays()) return;  // a dirty card or Settings page was kept
  // Fresh instance each time: the card loads its id at construction.
  if (game_card_ != nullptr) CloseGameEdit();
  SetGridControlsEnabled(false);
  game_card_ = new mira_gui::GameCard(id, library_, artwork_);
  SizeGameEditCard(game_card_);
  connect(game_card_, &mira_gui::GameCard::CloseRequested, this, &LibraryWindow::CloseGameEdit);
  connect(game_card_, &mira_gui::GameCard::PlayClicked, this, [this, id] { ToggleRunning(id); });
  game_edit_overlay_layout_->addWidget(game_card_, 0, 0, Qt::AlignCenter);
  root_stack_->setCurrentWidget(game_edit_overlay_);
  game_edit_overlay_->show();
  UpdateLibraryNavActive();
}

void LibraryWindow::CloseGameEdit() {
  game_edit_overlay_->hide();
  // Index 0 is chrome (root_stack_ only ever holds these two) -- raising it
  // back on top is cosmetic once the overlay is hidden, but keeps z-order
  // consistent for the next OpenGameDialog.
  root_stack_->setCurrentIndex(0);
  SetGridControlsEnabled(true);
  UpdateLibraryNavActive();
  // Torn down rather than left alive off-screen: IsDirty() on a discarded
  // form would otherwise still read dirty, and wrongly prompt again on the
  // next Ctrl+Q from the grid.
  if (game_card_ != nullptr) {
    game_edit_overlay_layout_->removeWidget(game_card_);
    game_card_->deleteLater();
    game_card_ = nullptr;
  }
  ShowNextCard();
}

void LibraryWindow::RequestCloseGameEdit() {
  if (game_card_ != nullptr) {
    game_card_->RequestClose();  // CloseRequested, once it may
  } else {
    CloseGameEdit();
  }
}

bool LibraryWindow::GameEditOpen() const {
  return game_edit_overlay_ != nullptr && game_edit_overlay_->isVisible();
}

void LibraryWindow::OpenSettings(const QString& focus_key) {
  // Already open: rebuilding would throw away whatever is half-typed.
  if (SettingsOpen()) {
    if (!focus_key.isEmpty() && settings_panel_ != nullptr) settings_panel_->FocusKey(focus_key);
    return;
  }

  // Fresh instance each time: starts synced to what's actually saved,
  // not stale edits left in the widgets from a discarded previous open.
  if (settings_page_ != nullptr) {
    content_stack_->removeWidget(settings_page_);
    settings_page_->deleteLater();
  }
  settings_page_ = BuildSettingsPage();
  content_stack_->addWidget(settings_page_);
  content_stack_->setCurrentWidget(settings_page_);
  SetSettingsChromeVisible(true);
  if (!focus_key.isEmpty()) settings_panel_->FocusKey(focus_key);
}

void LibraryWindow::CloseSettings() {
  content_stack_->setCurrentWidget(splitter_);
  SetSettingsChromeVisible(false);
  // Torn down rather than left alive off-screen: IsDirty() on a discarded
  // panel would otherwise still read dirty, and wrongly prompt again on the
  // next Ctrl+Q from the grid.
  if (settings_page_ != nullptr) {
    content_stack_->removeWidget(settings_page_);
    settings_page_->deleteLater();
    settings_page_ = nullptr;
    settings_panel_ = nullptr;
  }
  ShowNextCard();
}

bool LibraryWindow::SettingsOpen() const {
  return settings_page_ != nullptr && content_stack_->currentWidget() == settings_page_;
}

void LibraryWindow::SetSettingsChromeVisible(bool settings_open) {
  // The nav rows stay live: each leaves Settings on its way to its page.
  SetGridControlsEnabled(!settings_open);
  UpdateLibraryNavActive();
}

void LibraryWindow::SetGridControlsEnabled(bool enabled) {
  // The grid itself is what's leaving the screen either way -- nothing left
  // to preview.
  if (!enabled) HideHoverCard();
  // These act on a hidden grid. The sidebar's nav rows stay clickable, since
  // they're the way back out. The zoom slider is SyncZoom's.
  grid_page_->SetControlsEnabled(enabled);
  sidebar_->SetActionsEnabled(enabled);
  // Back from Settings onto a source page: the grid is still covered.
  if (enabled && (source_page_ != nullptr || runners_page_ != nullptr)) SetSourceControlsEnabled(false);
}

void LibraryWindow::UpdateLibraryNavActive() {
  // Only the pages it acts on show the tile size.
  if (zoom_ != nullptr) {
    const bool source_shown = content_stack_->currentWidget() == splitter_ && source_page_ != nullptr &&
                              main_stack_->currentWidget() == source_page_;
    zoom_->setVisible(GridShown() || source_shown);
  }
  const QString open_source = content_stack_->currentWidget() == splitter_ && source_page_ != nullptr
                                  ? source_page_->property("source_id").toString()
                                  : QString();
  if (sidebar_ != nullptr) {
    sidebar_->SetActive(GridShown() && !GameEditOpen(),
                        runners_page_ != nullptr && content_stack_->currentWidget() == splitter_,
                        open_source);
  }
  // Every page switch ends here, so the slider follows the page too.
  SyncZoom();
}

void LibraryWindow::RequestCloseSettings() {
  if (settings_panel_ == nullptr || !settings_panel_->IsDirty() ||
      mira_gui::notify::LeaveUnsaved(this, "Settings changed but not saved.", [this] {
        close_settings_after_save_ = true;
        settings_panel_->Save();  // SaveFinished, connected in BuildSettingsPage, closes on success
      })) {
    CloseSettings();
  }
}

QWidget* LibraryWindow::BuildSettingsPage() {
  auto* page = new QWidget(this);
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  mira_gui::SettingsPanel::Previews previews;
  previews.artwork = artwork_;
  for (const mira_gui::GameSummary* game : sidebar_->PinnedGames()) previews.pinned.push_back(*game);
  for (const mira_gui::GameSummary* game : library_->RecentlyPlayed(10))
    previews.recent.push_back(*game);
  settings_panel_ = new mira_gui::SettingsPanel(std::move(previews), page);
  close_settings_after_save_ = false;
  connect(settings_panel_, &mira_gui::SettingsPanel::LoadFailed, this, [this](QString error) {
    mira_gui::notify::Failed(this, "Could not load the settings.", error);
    CloseSettings();
  });
  connect(settings_panel_, &mira_gui::SettingsPanel::SaveFinished, this,
          [this](bool ok, QString error) {
            const bool close = std::exchange(close_settings_after_save_, false);
            if (!ok) {
              mira_gui::notify::Failed(this, "Could not save the settings.", error);
              return;
            }
            // The change bar going away is the feedback; no notice for a save the user just made.
            sidebar_->RefreshSources();
            if (close) CloseSettings();
          });
  connect(settings_panel_, &mira_gui::SettingsPanel::PrefsSaved, this, &LibraryWindow::ApplySettingsPrefs);
  layout->addWidget(settings_panel_, /*stretch=*/1);

  auto* header = new QWidget();
  auto* header_layout = new QHBoxLayout(header);
  header_layout->setContentsMargins(0, 0, 0, 2);
  header_layout->setSpacing(6);
  auto* back = new QToolButton(header);
  back->setAutoRaise(true);
  back->setIcon(mira_gui::icons::For(mira_gui::icons::Glyph::ArrowLeft));
  back->setToolTip("Back to the library");
  connect(back, &QToolButton::clicked, this, &LibraryWindow::RequestCloseSettings);
  header_layout->addWidget(back);
  auto* title = new QLabel("Settings", header);
  title->setProperty("role", "heading");
  header_layout->addWidget(title, /*stretch=*/1);
  settings_panel_->SetHeader(header);

  settings_panel_->AddSectionAction(
      "Library", "Moving games", "Move games into Mira's folders",
      "Moves each game's files into the library folder and its prefix into the prefix folder. "
      "Changing those folders does not move anything until you run this.",
      "Move games…", [this] {
        mira_gui::actions::RelocateLibrary(this, [this](const std::string& id) {
          const mira_gui::GameSummary* game = FindGame(id);
          return game != nullptr ? QString::fromStdString(game->name) : QString();
        });
      });
  settings_panel_->AddSectionAction(
      "Desktop entries", "Menu entries", "Regenerate desktop entries",
      "Rewrites Mira's desktop entries now, so changes to the desktop entry settings apply "
      "without waiting for the next library change.",
      "Regenerate", [this] { mira_gui::actions::SyncDesktopEntries(this); });
  settings_panel_->AddSectionAction("Desktop entries", "Menu entries", "Remove all desktop entries",
                                    "Turns off desktop entries and deletes every one Mira generated.",
                                    "Remove…", [this] { mira_gui::actions::RemoveAllDesktopEntries(this); });
  return page;
}

QWidget* LibraryWindow::BuildGameEditOverlay() {
  // Parented to nullptr here -- root_stack_->addWidget(overlay) reparents it
  // to central, same as any other widget added to a layout.
  auto* overlay = new mira_gui::ModalOverlay(nullptr);
  overlay->setObjectName("game_edit_overlay");
  // Plain black, not theme::window -- the theme's dark surfaces already
  // sit close to black, so tinting toward window barely dims anything.
  QColor scrim(0, 0, 0, 150);
  overlay->setStyleSheet(
      QString("QWidget#game_edit_overlay { background: rgba(%1, %2, %3, %4); }")
          .arg(scrim.red())
          .arg(scrim.green())
          .arg(scrim.blue())
          .arg(scrim.alpha()));
  overlay->hide();
  overlay->on_backdrop_clicked = [this] { RequestCloseGameEdit(); };

  game_edit_overlay_layout_ = new QGridLayout(overlay);
  game_edit_overlay_layout_->setContentsMargins(24, 24, 24, 24);
  return overlay;
}

QWidget* LibraryWindow::BuildSidebarCardOverlay() {
  auto* overlay = new mira_gui::ModalOverlay(nullptr);
  overlay->scrim = QColor(0, 0, 0, 150);
  // The sidebar stays bright: it is the preview.
  overlay->clear = [this, overlay] {
    QWidget* sidebar = splitter_->widget(0);
    return QRect(sidebar->mapTo(overlay->window(), QPoint(0, 0)) - overlay->mapTo(overlay->window(), QPoint(0, 0)),
                 sidebar->size());
  };
  overlay->hide();
  overlay->on_backdrop_clicked = [this] { CloseSidebarCard(); };
  sidebar_card_layout_ = new QGridLayout(overlay);
  return overlay;
}

void LibraryWindow::ShowSidebarCard(QWidget* card) {
  if (sidebar_card_overlay_ == nullptr) {
    sidebar_card_overlay_ = BuildSidebarCardOverlay();
    root_stack_->addWidget(sidebar_card_overlay_);
  }
  if (sidebar_card_ != nullptr) sidebar_card_->deleteLater();
  sidebar_card_ = card;
  // Centred over the content, beside the sidebar it changes.
  sidebar_card_layout_->setContentsMargins(splitter_->widget(0)->width() + mira_gui::kResizeMargin + 24, 24, 24, 24);
  sidebar_card_layout_->addWidget(card, 0, 0, Qt::AlignCenter);
  SetGridControlsEnabled(false);
  root_stack_->setCurrentWidget(sidebar_card_overlay_);
  sidebar_card_overlay_->show();
}

void LibraryWindow::OpenSidebarStyle() {
  if (!LeaveOverlays()) return;
  const auto copies = [](const std::vector<const mira_gui::GameSummary*>& games) {
    std::vector<mira_gui::GameSummary> out;
    for (const mira_gui::GameSummary* game : games) out.push_back(*game);
    return out;
  };
  auto* card =
      new mira_gui::SidebarStyleCard(sidebar_->StyleChoices(), copies(sidebar_->PinnedGames()),
                                     copies(library_->RecentlyPlayed(10)), artwork_);
  connect(card, &mira_gui::SidebarStyleCard::Changed, sidebar_, &mira_gui::Sidebar::SetStyleChoices);
  connect(card, &mira_gui::SidebarStyleCard::CloseRequested, this, &LibraryWindow::CloseSidebarCard);
  ShowSidebarCard(card);
}

void LibraryWindow::CloseSidebarCard() {
  if (!SidebarCardOpen()) return;
  sidebar_card_overlay_->hide();
  root_stack_->setCurrentIndex(0);
  SetGridControlsEnabled(true);
  if (sidebar_card_ != nullptr) {
    sidebar_card_->deleteLater();  // its own Close may be what got us here
    sidebar_card_ = nullptr;
  }
  ShowNextCard();
}

bool LibraryWindow::SidebarCardOpen() const {
  return sidebar_card_overlay_ != nullptr && sidebar_card_overlay_->isVisible();
}

// ~70% of the window, following it as it resizes.
void LibraryWindow::SizeGameEditCard(QWidget* card) {
  if (card != nullptr) card->setFixedSize(qRound(width() * 0.7), qRound(height() * 0.7));
}

bool LibraryWindow::LeaveOverlays() {
  CloseSidebarCard();  // nothing unsaved: every choice is stored as it's made
  if (SettingsOpen()) RequestCloseSettings();
  if (GameEditOpen()) RequestCloseGameEdit();
  // Still open: cancelled, or saving first.
  return !SettingsOpen() && !GameEditOpen();
}

void LibraryWindow::OpenSource(const mira_gui::SourceInfo& source) {
  if (!LeaveOverlays()) return;
  if (!ConfirmLeaveSource([this, source] { OpenSource(source); })) return;
  CloseRunners();
  if (source_page_ != nullptr) {
    main_stack_->removeWidget(source_page_);
    source_page_->deleteLater();
  }
  source_page_ = new mira_gui::SourcePage(source, library_, artwork_, downloads_, source_page_tabs_,
                                          SourceTileWidth(source.id), this);
  source_page_->SetDragSelectEnabled(drag_select_);
  source_page_->setProperty("source_id", source.id);
  connect(source_page_, &mira_gui::SourcePage::ZoomRequested, this,
          [this](int steps) { zoom_->setValue(zoom_->value() + steps * zoom_->pageStep()); });
  // The games themselves arrive as events.
  connect(source_page_, &mira_gui::SourcePage::LibraryChanged, this, [this, id = source.id] { sidebar_->NoteImported(id); });
  connect(source_page_, &mira_gui::SourcePage::OpenSettingsRequested, this,
          [this](const QString& key) { OpenSettings(key); });
  connect(source_page_, &mira_gui::SourcePage::Removed, this, [this, id = source.id] {
    if (mira_gui::SourceSettingsCard* card = source_page_->SettingsCard()) card->Discard();
    CloseSource();
    sidebar_->ForgetSource(id);
  });
  // Same rule as the grid's double-click: only a ready game has anything to launch.
  connect(source_page_, &mira_gui::SourcePage::GameMenuRequested, this,
          [this](const QString& id, const QPoint& pos, const QString& update_ref) {
            mira_gui::SourcePage* page = source_page_;
            menus_->ShowGameMenu(id.toStdString(), pos, [page, update_ref](QMenu& menu) {
              if (update_ref.isEmpty() || page == nullptr) return;
              QObject::connect(menu.addAction("Update"), &QAction::triggered, page,
                               [page, update_ref] { page->UpdateTitle(update_ref); });
            });
          });
  connect(source_page_, &mira_gui::SourcePage::BatchMenuRequested, this,
          [this](const QStringList& ids, const QPoint& pos) {
            std::vector<std::string> games;
            for (const QString& id : ids) games.push_back(id.toStdString());
            menus_->ShowBatchMenu(games, pos);
          });
  connect(source_page_, &mira_gui::SourcePage::PlayRequested, this, [this](const QString& id) {
    const mira_gui::GameSummary* game = FindGame(id.toStdString());
    if (game != nullptr && !double_click_play_) {
      ExplainDoubleClickOff(game->id);
    } else if (game != nullptr && mira_gui::CanPlayOrStop(*game)) {
      ToggleRunning(game->id);
    }
  });
  main_stack_->addWidget(source_page_);
  main_stack_->setCurrentWidget(source_page_);
  SetSourceControlsEnabled(false);
  UpdateLibraryNavActive();
}

bool LibraryWindow::ConfirmLeaveSource(std::function<void()> retry) {
  mira_gui::SourceSettingsCard* card = source_page_ != nullptr ? source_page_->SettingsCard() : nullptr;
  if (card == nullptr || !card->IsDirty()) return true;
  const bool leave =
      mira_gui::notify::LeaveUnsaved(this, "This source's settings changed but aren't saved.", [&] {
        // A failed save stays on the page, with its error on the card.
        connect(card, &mira_gui::SourceSettingsCard::SaveFinished, this,
                [retry = std::move(retry)](bool ok) {
                  if (ok) retry();
                },
                Qt::SingleShotConnection);
        card->Save();
      });
  // The nav row that was clicked checked itself; staying put unchecks it.
  UpdateLibraryNavActive();
  return leave;
}

bool LibraryWindow::CloseSource(std::function<void()> retry) {
  if (!retry) retry = [this] { CloseSource(); };
  if (!ConfirmLeaveSource(std::move(retry))) return false;
  main_stack_->setCurrentWidget(grid_page_);
  if (source_page_ != nullptr) {
    main_stack_->removeWidget(source_page_);
    source_page_->deleteLater();
    source_page_ = nullptr;
  }
  SetSourceControlsEnabled(true);
  UpdateLibraryNavActive();
  sidebar_->RefreshSources();  // a sign-in or launcher install there changes the order
  return true;
}

// The grid's search and filter leave with its page; the slider is SyncZoom's.
void LibraryWindow::SetSourceControlsEnabled(bool enabled) {
  if (!enabled) HideHoverCard();
}

bool LibraryWindow::GridShown() const {
  return content_stack_->currentWidget() == splitter_ && main_stack_->currentWidget() == grid_page_;
}

QString LibraryWindow::InstallText(const std::string& id) const {
  using State = mira_gui::DownloadTracker::State;
  const mira_gui::DownloadTracker::Entry* entry =
      downloads_->Find(mira_gui::DownloadTracker::KeyFor(mira_gui::DownloadTracker::Kind::Game, QString(),
                                                         QString::fromStdString(id)));
  if (entry == nullptr || entry->state != State::Running) return QString();
  if (entry->bytes <= 0) return "Installing…";
  return "Installing… " + mira_gui::SizeText(entry->bytes);
}

void LibraryWindow::DownloadChanged(const QString& key) {
  top_bar_->SetActivityCount(downloads_->RunningCount());

  // That game's row repaints with its new install text.
  if (key.startsWith("game:")) library_->Touch(key.mid(5).toStdString());

  // A store title's install shows on its search match, and once done it's no longer "not installed".
  const mira_gui::DownloadTracker::Entry* entry = downloads_->Find(key);
  if (entry != nullptr && entry->kind == mira_gui::DownloadTracker::Kind::Title) {
    grid_page_->RefreshOwnedStates();
    if (entry->state == mira_gui::DownloadTracker::State::Finished) owned_titles_->Refresh();
  }
}

void LibraryWindow::ShowGame(const std::string& id) {
  if (!LeaveOverlays()) return;
  if (source_page_ != nullptr && !CloseSource([this, id] { ShowGame(id); })) return;
  CloseRunners();
  if (!grid_page_->ShowGame(id)) OpenGameDialog(id);  // filtered out: its settings instead
}

void LibraryWindow::OpenManageSources() {
  if (!LeaveOverlays()) return;
  using mira_gui::ManageSourcesCard;
  auto* card = new ManageSourcesCard();
  card->SetEntries(sidebar_->SourceEntries());
  connect(card, &ManageSourcesCard::CloseRequested, this, &LibraryWindow::CloseSidebarCard);
  connect(card, &ManageSourcesCard::SidebarToggled, this,
          [this](const QString& id, bool shown) { sidebar_->SetSourceHidden(id, !shown); });
  connect(card, &ManageSourcesCard::OrderChanged, this, [this](const QStringList& ids) {
    sidebar_->SetSourceOrder(std::vector<QString>(ids.begin(), ids.end()));
  });
  connect(card, &ManageSourcesCard::EnabledToggled, sidebar_, &mira_gui::Sidebar::SetSourceEnabled);
  connect(card, &ManageSourcesCard::Imported, sidebar_, &mira_gui::Sidebar::NoteImported);
  connect(card, &ManageSourcesCard::Removed, sidebar_, &mira_gui::Sidebar::ForgetSource);
  connect(card, &mira_gui::ManageSourcesCard::OpenRequested, this, [this](const QString& id) {
    CloseSidebarCard();
    if (const mira_gui::SourceInfo* source = mira_gui::FindSourceInfo(id)) OpenSource(*source);
  });
  ShowSidebarCard(card);
}

void LibraryWindow::RowClicked(const std::string& id) {
  if (last_row_click_.isValid() && last_row_click_.elapsed() < QApplication::doubleClickInterval()) return;
  last_row_click_.start();
  const mira_gui::GameSummary* game = FindGame(id);
  if (game != nullptr && mira_gui::CanPlayOrStop(*game)) ToggleRunning(id);
}

void LibraryWindow::ShowLibrary() {
  if (SettingsOpen()) {
    RequestCloseSettings();
  } else if (GameEditOpen()) {
    RequestCloseGameEdit();
  } else if (source_page_ != nullptr) {
    CloseSource();
  } else if (runners_page_ != nullptr) {
    CloseRunners();
  }
  UpdateLibraryNavActive();
}

void LibraryWindow::HandleGameEvent(const std::string& type, const std::string& data, bool live) {
  if (type == "stream.gap") {
    // This stream fell behind mirad's buffer, so what changed meanwhile has to be asked for.
    RefreshGames();
    sidebar_->RefreshSources();
    downloads_->RecheckJobs();
    return;
  }
  if (type == "notification") {
    mira_gui::NotificationEvent event;
    if (live && mira_gui::events::ParseNotification(data, &event)) {
      const QString message = QString::fromStdString(event.message);
      // Only an error stays until dismissed; a warning ("no metadata found") is an answer, not an alarm.
      const auto level = mira_gui::notify::LevelFromString(QString::fromStdString(event.level));
      if (level == mira_gui::notify::Level::Error) {
        mira_gui::notify::Warn(this, message);
      } else {
        mira_gui::notify::Notice(this, message, level);
      }
    }
    return;
  }

  // Doesn't consume it: the toasts below still want installs.
  downloads_->HandleEvent(type, data);

  // A store install started from a search has nothing else on screen to say it
  // failed; a source page shows its own.
  if (mira_gui::StoreEvent store; live && type == "library.install.failed" &&
                                  mira_gui::events::ParseStoreEvent(type, data, &store) &&
                                  store.error.code != "cancelled" && !SourcePageShown()) {
    const mira_gui::DownloadTracker::Entry* entry = downloads_->Find(mira_gui::DownloadTracker::KeyFor(
        mira_gui::DownloadTracker::Kind::Title, QString::fromStdString(store.source), QString::fromStdString(store.ref)));
    const QString name = entry != nullptr ? downloads_->NameFor(*entry) : QString::fromStdString(store.ref);
    mira_gui::notify::FailedRequest(this, "Could not install " + name + ".", store.error);
  }

  if (mira_gui::StoreEvent art; mira_gui::events::ParseTitleArtworkEvent(type, data, &art)) {
    if (art.state == "ready") artwork_->TitleArtworkReady(art.source + "-" + art.ref);
    return;
  }

  if (mira_gui::InstallEvent install; mira_gui::events::ParseInstallEvent(type, data, &install)) {
    const mira_gui::GameSummary* game = FindGame(install.id);
    const QString name = game != nullptr ? QString::fromStdString(game->name) : QString("A game");
    if (!live) {
      // History: the grid below still picks up the result.
    } else if (install.state == "failed" && install.error.code != "cancelled") {
      mira_gui::notify::FailedRequest(this, "Could not install " + name + ".", install.error);
    } else if (install.state == "finished") {
      mira_gui::notify::Notice(this, name + " is installed.");
    }
    // The tile's install text follows the tracker (DownloadChanged); the
    // record itself arrives as game.updated.
    return;
  }

  if (type == "game.removed") {
    const std::string id = mira_gui::events::ParseRemovedId(data);
    if (!id.empty()) RemoveGame(id);
    return;
  }
  if (type == "games.removed") {
    const std::vector<std::string> ids = mira_gui::events::ParseRemovedIds(data);
    if (!ids.empty()) library_->Remove(ids);
    return;
  }

  if (type == "game.state") {
    mira_gui::GameStateEvent state;
    if (!mira_gui::events::ParseGameState(data, &state)) return;
    // The full record, `running` included; a bare {id, state} only moves running.
    if (mira_gui::GameSummary game; mira_gui::events::ParseGameSummary(data, &game) && !game.name.empty()) {
      game.running = state.state == "running";
      UpsertGames({game});
    } else {
      library_->SetRunning(state.id, state.state == "running");
    }
    // mirad only reports a real crash, a kill or a failed start; a non-zero exit alone is a normal quit.
    if (live && state.state == "crashed") {
      const mira_gui::GameSummary* crashed = FindGame(state.id);
      const QString name = crashed != nullptr ? QString::fromStdString(crashed->name) : QString("The game");
      const std::string& code = state.error.code;
      const QString what = code == "start_failed" ? " couldn't start." : code == "killed" ? " was killed." : " crashed.";
      mira_gui::notify::FailedRequest(this, name + what, state.error);
    }
    return;
  }

  if (type == "game.metadata_ready" || type == "game.metadata_failed") {
    // History's outcomes are already in what the grid fetched at startup.
    mira_gui::MetadataEvent event;
    if (!live || !mira_gui::events::ParseMetadataEvent(data, &event)) return;
    // The cover is fetched again only if this one changed it.
    artwork_->NoteArt(event.id, event.art);
    if (type == "game.metadata_ready") return;
    // The one failure worth interrupting for: it's fixable and never
    // transient: no SteamGridDB key means every non-Steam game keeps its
    // placeholder forever.
    if (event.code == "no_steamgriddb_key") {
      const bool asked_for = awaiting_metadata_.erase(event.id) > 0 || artwork_fetch_requested_;
      ShowSteamGridDbNotice(asked_for, event.error);
      return;
    }

    // Everything else mirad already reports as a `notification` event when
    // the fetch was announced.
    awaiting_metadata_.erase(event.id);
    return;
  }

  if (type == "game.artwork_selected") {
    mira_gui::ArtworkSelectEvent event;
    if (!live || !mira_gui::events::ParseArtworkSelectEvent(data, &event)) return;
    if (event.slot == "cover") {
      artwork_->NoteArt(event.id, event.art);
    } else if (event.slot == "hero") {
      if (game_card_ != nullptr) game_card_->RefreshHero(event.id);
    }
    return;
  }

  if (type == "game.installer_leftover") {
    // Asked once, as it happens; history would ask again after every reconnect.
    mira_gui::InstallerLeftoverEvent event;
    if (live && mira_gui::events::ParseInstallerLeftover(data, &event)) OfferInstallerDelete(event);
    return;
  }

  if (type == "game.install_detected") {
    // History's would ask again after every reconnect.
    mira_gui::InstallDetectedEvent event;
    if (!live || !mira_gui::events::ParseInstallDetected(data, &event)) return;
    QTimer::singleShot(0, this, [this, event] { AskAboutInstall(event); });
    return;
  }

  if (type == "game.launched") {
    // mirad hands a Steam game to steam://rungameid and says whether it is
    // watching the process. Tracked: leave it alone, game.state is coming.
    // Untracked: clear it, since nothing will ever say it stopped.
    mira_gui::GameLaunchedEvent launched;
    if (mira_gui::events::ParseGameLaunched(data, &launched) && !launched.tracked) {
      library_->SetRunning(launched.id, false);
    }
    return;
  }

  // Explicitly the two event types that carry a game record, not "anything
  // left over": mirad also publishes runners.download.* and tricks.* here.
  if (type == "games.updated") {
    std::vector<mira_gui::GameSummary> games;
    if (mira_gui::events::ParseGameSummaries(data, &games)) UpsertGames(games);
    return;
  }
  if (type != "game.added" && type != "game.updated") return;

  mira_gui::GameSummary game;
  if (mira_gui::events::ParseGameSummary(data, &game)) UpsertGames({game});

  // A new installer asks to be run instead of opening its settings, unless mirad already runs it.
  if (live && type == "game.added" && game.status == "needs_install" && !game.id.empty()) {
    if (!mira_gui::events::ParseAutoInstall(data)) OfferInstall(game.id);
    return;
  }

  // open_config_on_add: open a newly detected game's settings to check them.
  // Only for a lone arrival; a scan that finds several opens nothing rather
  // than stacking cards. Never for history, which would open one at startup.
  if (live && type == "game.added" && mira_gui::events::ParseOpenConfig(data) && !game.id.empty()) {
    pending_added_.push_back(game.id);
    if (added_timer_ == nullptr) {
      added_timer_ = new QTimer(this);
      added_timer_->setSingleShot(true);
      added_timer_->setInterval(1500);
      connect(added_timer_, &QTimer::timeout, this, [this] {
        const std::vector<std::string> added = std::move(pending_added_);
        pending_added_.clear();
        if (added.size() == 1 && GridShown() && !GameEditOpen() && FindGame(added.front()) != nullptr) {
          OpenGameDialog(added.front());
        }
      });
    }
    added_timer_->start();
  }
}
