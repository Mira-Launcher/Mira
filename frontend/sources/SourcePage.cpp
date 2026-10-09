#include "SourcePage.h"

#include <algorithm>

#include <QEvent>
#include <QGridLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStandardItemModel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "../client/Events.h"
#include "../client/api/Artwork.h"
#include "../client/api/Library.h"
#include "../client/api/Stores.h"
#include "../dialogs/AddManualGameDialog.h"
#include "../dialogs/LogWindow.h"
#include "ItchCollectionsCard.h"
#include "../activity/DownloadTracker.h"
#include "../app/ErrorHelp.h"
#include "../app/Notify.h"
#include "../client/EventHub.h"
#include "../library/ArtworkStore.h"
#include "../library/CoverArt.h"
#include "../library/GameActions.h"
#include "../library/GameLibraryModel.h"
#include "../library/GameTileDelegate.h"
#include "../library/HoverCard.h"
#include "../library/RatingChips.h"
#include "../library/TileGrid.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "../widgets/Labels.h"
#include "../widgets/ModalOverlay.h"
#include "../widgets/ProgressRail.h"
#include "../widgets/Scrolling.h"
#include "../widgets/TabRow.h"
#include "SourceRemoval.h"
#include "SourceSettingsCard.h"
#include "SourceSetupCard.h"
#include "SourceText.h"

namespace mira_gui {

namespace {
QStringList ToQStringList(const std::vector<std::string>& values) {
  QStringList out;
  for (const std::string& value : values) out << QString::fromStdString(value);
  return out;
}
}  // namespace

SourcePage::SourcePage(const SourceInfo& source, GameLibraryModel* library, ArtworkStore* artwork,
                       DownloadTracker* downloads, int tile_width, QWidget* parent)
    : QWidget(parent),
      source_(source),
      id_(source.id.toStdString()),
      library_(library),
      artwork_(artwork),
      downloads_(downloads),
      tile_(tile_width, tile_width * 3 / 2) {
  games_ = new GameFilterProxy(library_, this);
  games_->SetSource(id_);
  owned_model_ = new QStandardItemModel(this);
  // Covers drawn quickly while the zoom moved, drawn again now it has stopped.
  connect(artwork_, &ArtworkStore::QuickScalingEnded, this, [this] {
    if (library_grid_ != nullptr) library_grid_->viewport()->update();
    if (owned_grid_ != nullptr) RebuildOwnedTiles();
  });
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);

  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  SetUpScrolling(scroll, this);
  auto* content = new QWidget();
  content_layout_ = new QVBoxLayout(content);
  content_layout_->setContentsMargins(22, 14, 22, 16);
  content_layout_->setSpacing(14);
  content_layout_->addWidget(BuildTopRow());
  setup_card_ = new SourceSetupCard(source_, this);
  connect(setup_card_, &SourceSetupCard::StatusChanged, this, &SourcePage::RefreshStatus);
  connect(setup_card_, &SourceSetupCard::LauncherInstallStarted, this, [this] {
    launcher_installing_ = true;
    UpdateStatusLine();
  });
  connect(setup_card_, &SourceSetupCard::LauncherInstallFailed, this, [this] {
    launcher_installing_ = false;
    UpdateStatusLine();
  });
  content_layout_->addWidget(setup_card_);
  if (id_ != "humble") content_layout_->addWidget(BuildLibrarySection());
  if (id_ != "humble" && HasOwned()) content_layout_->addWidget(MakeDivider(content, Qt::Horizontal));
  if (HasOwned()) content_layout_->addWidget(BuildOwnedSection());
  content_layout_->addStretch(1);
  scroll->setWidget(content);
  // A click on the page's own background deselects, like one between tiles.
  content->installEventFilter(this);
  content_ = content;
  outer->addWidget(scroll);

  connect(EventHub::Instance(), &EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) {
            if (live) HandleEvent(type, data);
          });
  connect(downloads_, &DownloadTracker::Changed, this, [this](const QString& key) {
    if (owned_grid_ != nullptr && (key.isEmpty() || key.startsWith(source_.id + ":"))) RebuildOwnedTiles();
  });
  connect(library_, &GameLibraryModel::Changed, this, &SourcePage::LibraryUpdated);
  LibraryUpdated();

  RefreshStatus();
  // mirad answers from the list it stored, so a store's doesn't wait for its status. Humble's
  // purchases are asked for live, once signed in.
  if (id_ == "steam" || (IsStore() && id_ != "humble")) RefreshOwned();
}

bool SourcePage::eventFilter(QObject* watched, QEvent* event) {
  if (watched == modal_card_ && modal_card_ != nullptr && event->type() == QEvent::LayoutRequest && SettingsModalOpen()) {
    QMetaObject::invokeMethod(this, &SourcePage::FitSettingsModal, Qt::QueuedConnection);
  }
  if (watched == content_ && event->type() == QEvent::MouseButtonPress) {
    for (TileGrid* grid : {library_grid_, owned_grid_}) {
      if (grid == nullptr) continue;
      grid->clearSelection();
      grid->setCurrentIndex(QModelIndex());
    }
  }
  return QWidget::eventFilter(watched, event);
}

bool SourcePage::HasImport() const { return !CopyFor(id_).import_button.isEmpty(); }

// Every page but Local has both sections; ones that can't list what the account owns say so.
bool SourcePage::HasOwned() const { return id_ != "local"; }

bool SourcePage::ListsOwned() const { return IsStore() || id_ == "steam" || id_ == "office"; }

bool SourcePage::IsOwnGame(const GameSummary& game) const { return SourceIdOf(game.source) == id_; }

// No title: the sidebar already says which source this is. One row holds the
// status and every action.
QWidget* SourcePage::BuildTopRow() {
  tabs_ = new TabRow(this);
  tabs_->SetTabsVisible(false);

  status_line_ = new QLabel(tabs_);
  status_line_->setObjectName("page_status");
  status_line_->setTextFormat(Qt::RichText);
  status_line_->setToolTip(CopyFor(id_).blurb);
  tabs_->SetTrailing(status_line_);

  // Launchers: open it. Stores: sign out.
  banner_primary_ = new QPushButton(tabs_);
  banner_primary_->setVisible(false);
  connect(banner_primary_, &QPushButton::clicked, this, [this] {
    banner_primary_->setEnabled(false);
    if (IsLauncher()) {
      api::OpenLauncherAsync(this, id_, [this](StoreActionResult result) {
        banner_primary_->setEnabled(true);
        if (!result.ok) {
          setup_card_->ShowError("Could not open " + source_.name + ".", result.error);
        }
      });
      return;
    }
    api::SignOutStoreAsync(this, id_, [this](StoreActionResult result) {
      banner_primary_->setEnabled(true);
      if (!result.ok) {
        setup_card_->ShowError("Could not sign out.", result.error);
        return;
      }
      RefreshStatus();
    });
  });
  tabs_->SetTrailing(banner_primary_);

  // Steam can't report the status back, so neither button shows one as current.
  if (id_ == "steam") {
    const auto status_button = [this](const QString& label, icons::Glyph glyph, icons::Role color,
                                      const std::string& status) {
      auto* button = new QPushButton(label, tabs_);
      icons::Follow(button, glyph, color);
      connect(button, &QPushButton::clicked, this, [this, button, status] {
        button->setEnabled(false);
        api::SetSteamStatusAsync(this, status, [this, button](StoreActionResult result) {
          button->setEnabled(true);
          if (!result.ok)
            notify::FailedRequest(this, "Could not set your Steam status.", result.error);
        });
      });
      tabs_->SetTrailing(button);
    };
    status_button("Go online", icons::Glyph::Dot, &theme::Tokens::running, "online");
    status_button("Go invisible", icons::Glyph::EyeSlash, &theme::Tokens::text, "invisible");
  }

  import_button_ = new QPushButton(CopyFor(id_).import_button, tabs_);
  icons::Follow(import_button_, icons::Glyph::Refresh);
  // Stores and launchers show it once set up (ApplyStoreStatus/ApplyLauncher).
  import_button_->setVisible(HasImport() && !IsStore() && !IsLauncher());
  connect(import_button_, &QPushButton::clicked, this, &SourcePage::Import);
  tabs_->SetTrailing(import_button_);

  settings_button_ = new QToolButton(tabs_);
  icons::Follow(settings_button_, icons::Glyph::Settings);
  settings_button_->setToolTip(source_.name + " settings");
  settings_button_->setAutoRaise(true);
  connect(settings_button_, &QToolButton::clicked, this, &SourcePage::OpenSettingsModal);
  tabs_->SetTrailing(settings_button_);
  // Local has no settings of its own, and nothing to remove.
  settings_button_->setVisible(id_ != "local");

  more_button_ = new QToolButton(tabs_);
  more_button_->setText("⋯");
  more_button_->setToolTip("More");
  more_button_->setAutoRaise(true);
  more_button_->setPopupMode(QToolButton::InstantPopup);
  auto* more_menu = new QMenu(more_button_);
  connect(more_menu, &QMenu::aboutToShow, this, [this, more_menu] { FillMoreMenu(more_menu); });
  more_button_->setMenu(more_menu);
  tabs_->SetTrailing(more_button_);
  more_button_->setVisible(id_ != "local");

  filter_ = new QLineEdit(tabs_);
  filter_->setPlaceholderText("Filter…");
  filter_->setClearButtonEnabled(true);
  connect(filter_, &QLineEdit::textChanged, this, &SourcePage::ApplyFilter);
  tabs_->SetSearch(filter_);
  return tabs_;
}

// A dialog over the page: the games underneath stay where they are.
void SourcePage::OpenSettingsModal() {
  if (settings_card_ == nullptr) {
    settings_card_ = new SourceSettingsCard(source_, this);
    connect(settings_card_, &SourceSettingsCard::OpenSettingsRequested, this, &SourcePage::OpenSettingsRequested);
    settings_card_->setMaximumWidth(560);
    auto* close = new QToolButton(settings_card_);
    icons::Follow(close, icons::Glyph::Close);
    close->setToolTip("Close");
    close->setAutoRaise(true);
    connect(close, &QToolButton::clicked, this, &SourcePage::CloseSettingsModal);
    settings_card_->Header()->addWidget(close);
  } else {
    settings_card_->Refresh();
  }
  ShowModal(settings_card_);
}

void SourcePage::OpenCollectionsModal() {
  if (collections_card_ == nullptr) {
    collections_card_ = new ItchCollectionsCard(this);
    collections_card_->setMaximumWidth(560);
    connect(collections_card_, &ItchCollectionsCard::Changed, this, [this] { RefreshOwned(); });
    auto* close = new QToolButton(collections_card_);
    icons::Follow(close, icons::Glyph::Close);
    close->setToolTip("Close");
    close->setAutoRaise(true);
    connect(close, &QToolButton::clicked, this, &SourcePage::CloseSettingsModal);
    collections_card_->Header()->addWidget(close);
  }
  collections_card_->Refresh();
  ShowModal(collections_card_);
}

void SourcePage::ShowModal(mira_gui::SettingsCard* card) {
  if (settings_overlay_ == nullptr) {
    settings_overlay_ = new ModalOverlay(this);
    settings_overlay_->scrim = QColor(0, 0, 0, 150);
    settings_overlay_->setFocusPolicy(Qt::StrongFocus);
    settings_overlay_->on_backdrop_clicked = [this] { CloseSettingsModal(); };
    auto* scroll = new QScrollArea(settings_overlay_);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    SetUpScrolling(scroll, settings_overlay_);  // set up after the page's, so it's asked first
    // Only the viewport: the card keeps its own background.
    scroll->setStyleSheet("QScrollArea, QScrollArea > QWidget { background: transparent; }");
    // Centered both ways; FitSettingsModal sizes it, and a card taller than the window scrolls.
    settings_scroll_ = scroll;
    auto* column = new QVBoxLayout(settings_overlay_);
    column->setContentsMargins(32, 32, 32, 32);
    column->addStretch(1);
    column->addWidget(scroll, 0, Qt::AlignHCenter);
    column->addStretch(1);
  }
  if (settings_scroll_->widget() != card) {
    if (QWidget* shown = settings_scroll_->takeWidget()) {
      shown->hide();
      shown->setParent(this);
    }
    settings_scroll_->setWidget(card);
    card->show();
    // Its rows arrive from mirad after it opens: the dialog follows its height.
    card->installEventFilter(this);
  }
  modal_card_ = card;
  settings_overlay_->setGeometry(rect());
  FitSettingsModal();
  settings_overlay_->show();
  settings_overlay_->raise();
  settings_overlay_->setFocus();
}

bool SourcePage::SettingsModalOpen() const { return settings_overlay_ != nullptr && settings_overlay_->isVisible(); }

void SourcePage::CloseSettingsModal() {
  if (!SettingsModalOpen()) return;
  if (modal_card_ == settings_card_ && settings_card_->IsDirty()) {
    const bool leave = notify::LeaveUnsaved(this, "This source's settings changed but aren't saved.", [this] {
      connect(settings_card_, &SourceSettingsCard::SaveFinished, this,
              [this](bool ok) {
                if (!ok) return;
                settings_overlay_->hide();
                            },
              Qt::SingleShotConnection);
      settings_card_->Save();
    });
    if (!leave) return;
    settings_card_->Discard();
  }
  settings_overlay_->hide();
}

// The card at its natural size, 560 wide at most, shrunk to fit a small window.
void SourcePage::FitSettingsModal() {
  if (settings_scroll_ == nullptr || modal_card_ == nullptr) return;
  constexpr int kMargin = 32;
  const int width = std::min(560, std::max(280, this->width() - 2 * kMargin));
  modal_card_->setFixedWidth(width);
  const int wanted = modal_card_->heightForWidth(width) > 0 ? modal_card_->heightForWidth(width)
                                                               : modal_card_->sizeHint().height();
  settings_scroll_->setFixedWidth(width + (wanted > height() - 2 * kMargin ? settings_scroll_->style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0));
  settings_scroll_->setFixedHeight(std::min(wanted, std::max(120, height() - 2 * kMargin)));
}

void SourcePage::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  if (settings_overlay_ != nullptr) {
    settings_overlay_->setGeometry(rect());
    FitSettingsModal();
  }
}

void SourcePage::FillMoreMenu(QMenu* menu) {
  menu->clear();
  const QString tool = CopyFor(id_).tool;
  if (IsStore() && tool_installed_) {
    const QString version = tool_version_.empty() ? QString() : " (" + QString::fromStdString(tool_version_) + ")";
    QAction* update = menu->addAction(icons::For(icons::Glyph::Download), "Update " + tool + version, this,
                                      &SourcePage::UpdateTool);
    update->setEnabled(!tool_updating_);
    update->setToolTip("Download the latest release of " + tool + " again.");
  }
  if (IsLauncher() || IsStore()) {
    menu->addAction("View setup log", this, [this] { LogWindow::Open(this, "setup:" + source_.id, source_.name + " setup"); });
  }
  if (IsLauncher() && launcher_installed_ && !launcher_game_id_.empty()) {
    const std::string game_id = launcher_game_id_;
    menu->addAction(icons::For(icons::Glyph::Home), "Open prefix folder", this, [this] {
      actions::OpenInstallFolder(this, launcher_prefix_);
    });
    menu->addAction(icons::For(icons::Glyph::Wrench), "Winetricks…", this,
                    [this, game_id] { actions::RunWinetricks(this, game_id, source_.name); });
    menu->addAction("Run a program in its prefix…", this, [this, game_id] {
      actions::RunInPrefix(this, game_id, launcher_prefix_ + "/drive_c", source_.name);
    });
    menu->addAction("View launcher log", this, [this, game_id] { actions::ViewLog(this, game_id, source_.name); });
  }

  // What removing does differs per kind; RemoveSource spells it out before anything happens.
  const bool removable = library_count_ > 0 || (IsStore() ? tool_installed_ || authenticated_
                                                : IsLauncher() ? launcher_installed_
                                                               : true);
  if (removable) {
    if (!menu->isEmpty()) menu->addSeparator();
    const QString label = IsLauncher() ? "Uninstall " + source_.name + "…"
                          : IsStore()  ? "Remove " + source_.name + "…"
                                       : "Remove from Mira…";
    menu->addAction(icons::For(icons::Glyph::Trash, theme::Current().error), label, this, [this] {
      RemoveSource(this, source_, [this] { emit Removed(); });
    });
  }
  if (menu->isEmpty()) menu->addAction("Nothing to manage until it's set up")->setEnabled(false);
}

void SourcePage::UpdateTool() {
  tool_updating_ = true;
  UpdateStatusLine();
  api::SetupStoreToolAsync(this, id_, [this](StoreActionResult result) {
    tool_updating_ = false;
    if (result.ok) {
      RefreshStatus();
      return;
    }
    UpdateStatusLine();
    setup_card_->ShowError("Could not update " + CopyFor(id_).tool + ".", result.error);
  });
}

QWidget* SourcePage::BuildLibrarySection() {
  auto* section = new QWidget(this);
  library_section_ = section;
  auto* layout = new QVBoxLayout(section);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);

  auto* header = new QHBoxLayout();
  library_heading_ = new QLabel("In your library", section);
  library_heading_->setProperty("role", "heading");
  header->addWidget(library_heading_);
  header->addStretch(1);
  import_result_ = MakeLabel(section, QString(), "muted");
  import_result_->setWordWrap(false);
  import_result_->setVisible(false);
  header->addWidget(import_result_);
  layout->addLayout(header);

  library_empty_ = MakeLabel(section, QString(), "muted");
  layout->addWidget(library_empty_);

  library_grid_ = new TileGrid(tile_, artwork_, section);
  static_cast<GameTileDelegate*>(library_grid_->itemDelegate())->SetShowSource(false);
  library_grid_->setModel(games_);
  library_grid_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(library_grid_, &QWidget::customContextMenuRequested, this, &SourcePage::ShowLibraryMenu);
  connect(library_grid_, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& index) {
    emit PlayRequested(index.data(GameTileDelegate::IdRole).toString());
  });
  library_grid_->on_hover = [this](const QModelIndex& index) { ShowHoverCard(library_grid_, index); };
  library_grid_->on_ctrl_wheel = [this](int steps) { emit ZoomRequested(steps); };
  layout->addWidget(library_grid_);
  return section;
}

QWidget* SourcePage::BuildOwnedSection() {
  owned_section_ = new QWidget(this);
  owned_available_ = id_ == "steam" || !ListsOwned();
  auto* layout = new QVBoxLayout(owned_section_);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);

  auto* header = new QHBoxLayout();
  header->setSpacing(8);
  owned_heading_ = new QLabel(id_ == "humble" ? "Your purchases" : "Not installed", owned_section_);
  owned_heading_->setProperty("role", "heading");
  header->addWidget(owned_heading_, 0, Qt::AlignBaseline);
  owned_count_ = MakeLabel(owned_section_, QString(), "muted", /*wrap=*/false);
  header->addWidget(owned_count_, 0, Qt::AlignBaseline);
  header->addStretch(1);
  clear_filters_ = new QPushButton("Clear filters", owned_section_);
  clear_filters_->setObjectName("text_button");
  QSizePolicy keep = clear_filters_->sizePolicy();
  keep.setRetainSizeWhenHidden(true);
  clear_filters_->setSizePolicy(keep);
  clear_filters_->setVisible(false);
  header->addWidget(clear_filters_);
  owned_refresh_ = new QPushButton("Refresh", owned_section_);
  owned_refresh_->setToolTip("Ask " + source_.name + " again now");
  icons::Follow(owned_refresh_, icons::Glyph::Refresh);
  connect(owned_refresh_, &QPushButton::clicked, this, [this] { RefreshOwned(/*fresh=*/true); });
  if (id_ == "itch") {
    auto* collections = new QPushButton("Manage collections…", owned_section_);
    collections->setToolTip("Show games from itch.io collections here. Add your own or any collection by link.");
    connect(collections, &QPushButton::clicked, this, &SourcePage::OpenCollectionsModal);
    header->addWidget(collections);
  }
  header->addWidget(owned_refresh_);
  layout->addLayout(header);

  // Always there, so nothing below moves when it starts or stops.
  owned_rail_ = new ProgressRail(owned_section_);
  owned_rail_->setFixedHeight(3);
  QSizePolicy keep_rail = owned_rail_->sizePolicy();
  keep_rail.setRetainSizeWhenHidden(true);
  owned_rail_->setSizePolicy(keep_rail);
  owned_rail_->SetProgress(0);
  owned_rail_->setVisible(false);
  layout->addWidget(owned_rail_);

  if (id_ == "steam" || (IsStore() && id_ != "humble")) {
    chips_ = new RatingChips(owned_section_);
    chips_->setVisible(false);
    connect(chips_, &RatingChips::Changed, this, &SourcePage::ApplyFilter);
    connect(clear_filters_, &QPushButton::clicked, chips_, &RatingChips::Clear);
    layout->addWidget(chips_);
  }

  owned_skeleton_ = new QWidget(owned_section_);
  auto* skeleton = new QHBoxLayout(owned_skeleton_);
  skeleton->setContentsMargins(0, 0, 0, 0);
  skeleton->setSpacing(theme::Current().tile_spacing);
  for (int i = 0; i < 3; ++i) {
    auto* tile = new QWidget(owned_skeleton_);
    tile->setObjectName("tile_skeleton");
    tile->setAttribute(Qt::WA_StyledBackground);
    tile->setFixedSize(tile_);
    skeleton->addWidget(tile);
  }
  skeleton->addStretch(1);
  owned_skeleton_->setVisible(false);
  layout->addWidget(owned_skeleton_);

  owned_note_ = MakeLabel(owned_section_, QString(), "muted");
  owned_note_->setVisible(false);
  layout->addWidget(owned_note_);
  if (id_ == "steam") {
    steam_settings_ = new QPushButton("Open Steam settings", owned_section_);
    steam_settings_->setVisible(false);
    connect(steam_settings_, &QPushButton::clicked, this,
            [this] { emit OpenSettingsRequested("steam.web_api_key"); });
    auto* row = new QHBoxLayout();
    row->addWidget(steam_settings_);
    row->addStretch(1);
    layout->addLayout(row);
  }
  if (id_ != "humble") {
    art_key_ = new QPushButton("Add a SteamGridDB key for covers", owned_section_);
    icons::Follow(art_key_, icons::Glyph::Image);
    art_key_->setVisible(false);
    connect(art_key_, &QPushButton::clicked, this, [this] { emit OpenSettingsRequested(art_key_setting_); });
    auto* row = new QHBoxLayout();
    row->addWidget(art_key_);
    row->addStretch(1);
    layout->addLayout(row);
  }

  if (!ListsOwned()) {
    owned_refresh_->setVisible(false);
    if (art_key_ != nullptr) art_key_->setVisible(false);
    const QString what = IsLauncher() ? source_.name + " doesn't share what you own. Games you install through it show up in your library above."
                                      : source_.name + " doesn't list games you own. What it installs shows up in your library above.";
    ShowLine(owned_note_, what, "muted");
  }

  owned_grid_ = new TileGrid(tile_, artwork_, owned_section_);
  // The page already says which source these are from.
  static_cast<GameTileDelegate*>(owned_grid_->itemDelegate())->SetShowSource(false);
  owned_grid_->setModel(owned_model_);
  owned_grid_->on_hover = [this](const QModelIndex& index) { ShowHoverCard(owned_grid_, index); };
  owned_grid_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(owned_grid_, &QWidget::customContextMenuRequested, this, &SourcePage::ShowOwnedMenu);
  owned_grid_->on_ctrl_wheel = [this](int steps) { emit ZoomRequested(steps); };
  owned_grid_->on_action = [this](const QModelIndex& index) {
    const QString ref = index.data(GameTileDelegate::IdRole).toString();
    if (id_ != "humble") {
      StartInstall(ref, /*update=*/false);
      return;
    }
    if (humble_paths_.contains(ref)) {
      AddManualGameDialog dialog(this);
      dialog.Prefill(humble_paths_.value(ref), index.data(GameTileDelegate::NameRole).toString());
      dialog.exec();
      return;
    }
    owned_state_.insert(ref, "Downloading…");
    RebuildOwnedTiles();
    api::DownloadHumbleBundleAsync(this, ref.toStdString(), [this, ref](HumbleDownloadResult r) {
      if (r.ok) {
        owned_state_.remove(ref);
        humble_paths_.insert(ref, QString::fromStdString(r.path));
      } else if (r.error.code == "nothing_to_download") {
        owned_state_.insert(ref, "Nothing to download");  // e.g. only a Steam key
      } else {
        owned_state_.remove(ref);
        ShowError(owned_note_, "The download failed.", r.error);
      }
      RebuildOwnedTiles();
    });
  };
  // Double-click does what the tile's button does, while it's clickable.
  connect(owned_grid_, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& index) {
    if (index.data(GameTileDelegate::ActionEnabledRole).toBool() && owned_grid_->on_action) owned_grid_->on_action(index);
  });
  layout->addWidget(owned_grid_);
  return owned_section_;
}

void SourcePage::LibraryUpdated() {
  library_count_ = static_cast<int>(
      std::ranges::count_if(library_->Games(), [this](const GameSummary& game) { return IsOwnGame(game); }));
  if (library_heading_ != nullptr) library_heading_->setText(CountedHeading("In your library", library_count_));
  UpdateStatusLine();
  if (library_grid_ != nullptr) library_grid_->FitHeight();
}

void SourcePage::SetTileWidth(int width) {
  const QSize tile(width, width * 3 / 2);
  if (tile == tile_) return;
  tile_ = tile;
  // The library grid's delegate draws its covers at its own size.
  if (library_grid_ != nullptr) library_grid_->SetTileSize(tile_);
  if (owned_grid_ != nullptr) {
    owned_grid_->SetTileSize(tile_);
    RebuildOwnedTiles();
  }
  if (owned_skeleton_ != nullptr) {
    for (QWidget* placeholder : owned_skeleton_->findChildren<QWidget*>("tile_skeleton")) placeholder->setFixedSize(tile_);
  }
}

void SourcePage::UpdateCover(const QString& id) {
  // A not-installed title's cover is keyed "<source>-<ref>". The library
  // grid's own tiles repaint from the shared model.
  const QString prefix = source_.id + "-";
  if (owned_grid_ == nullptr || id_ == "humble" || !id.startsWith(prefix)) return;
  const QString ref = id.mid(prefix.size());
  for (int row = 0; row < owned_model_->rowCount(); ++row) {
    QStandardItem* item = owned_model_->item(row);
    if (item->data(GameTileDelegate::IdRole).toString() != ref) continue;
    item->setData(artwork_->TitleCover(source_.id, ref, item->data(GameTileDelegate::NameRole).toString(), tile_,
                                       devicePixelRatioF()),
                  Qt::DecorationRole);
  }
  // Its details came with the cover, so its tier, reviews and tags may be new.
  if (tiers_.contains(ref) && tags_.contains(ref) && reviews_.contains(ref)) return;
  api::GetTitleMetadataAsync(this, id_, ref.toStdString(), [this, ref](GameMetadataResult result) {
    if (!result.ok) return;
    const GameMetadata& metadata = result.metadata;
    if (!metadata.steam_tags.empty()) tags_.insert(ref, ToQStringList(metadata.steam_tags));
    if (!metadata.protondb_tier.empty()) tiers_.insert(ref, QString::fromStdString(metadata.protondb_tier));
    if (!metadata.review_summary.empty()) reviews_.insert(ref, QString::fromStdString(metadata.review_summary));
    if (metadata.review_percent >= 0) review_percents_.insert(ref, metadata.review_percent);
    RebuildOwnedTiles();
  });
}

void SourcePage::RefreshStatus() {
  if (IsStore()) {
    api::GetStoreStatusAsync(this, id_, [this](StoreStatusResult s) { ApplyStoreStatus(s); });
  } else if (IsLauncher()) {
    api::GetLaunchersAsync(this, [this](LaunchersResult result) {
      if (!result.ok) {
        setup_card_->ShowError("Could not ask mirad about " + source_.name + ".", result.error);
        return;
      }
      for (const LauncherInfo& launcher : result.launchers) {
        if (launcher.id == id_) ApplyLauncher(launcher);
      }
    });
  } else {
    UpdateStatusLine();
  }
}

void SourcePage::ApplyStoreStatus(const StoreStatusResult& status) {
  if (!status.ok) {
    setup_card_->ShowError("Could not ask mirad about " + source_.name + ".", status.error);
    return;
  }
  const bool was_authenticated = authenticated_;
  tool_installed_ = status.tool_installed;
  tool_version_ = status.tool_version;
  authenticated_ = status.authenticated;
  account_ = status.account;
  setup_card_->ShowStore(tool_installed_, authenticated_);

  banner_primary_->setText("Sign out");
  banner_primary_->setVisible(authenticated_ && id_ != "humble");
  if (import_button_ != nullptr) import_button_->setVisible(tool_installed_ && HasImport());
  owned_available_ = tool_installed_ && authenticated_;
  if (owned_section_ != nullptr && !owned_available_) {
    owned_refresh_->setEnabled(false);
    // Whatever was listed before signing out isn't this account's to install.
    owned_.clear();
    RebuildOwnedTiles();
    ShowLine(owned_note_, "Sign in to " + source_.name + " to see the games you own.", "muted");
  } else if (owned_section_ != nullptr) {
    owned_refresh_->setEnabled(!owned_loading_);
  }
  // Listed at once for a store already signed in; Humble and a new sign-in wait for the status.
  const bool listed_at_open = id_ != "humble" && !status_known_;
  if (owned_section_ != nullptr && authenticated_ && (!was_authenticated && !listed_at_open)) RefreshOwned();
  status_known_ = true;
  UpdateStatusLine();
}

void SourcePage::ApplyLauncher(const LauncherInfo& launcher) {
  const bool was_installed = launcher_installed_;
  launcher_installed_ = launcher.installed;
  launcher_installing_ = launcher.install_state == "running";
  launcher_game_id_ = launcher.game_id;
  launcher_prefix_ = launcher.prefix;
  // Its runner row only works once there's a prefix.
  if (launcher_installed_ != was_installed && settings_card_ != nullptr) settings_card_->Refresh();
  setup_card_->ShowLauncher(launcher, launcher_installing_);
  banner_primary_->setText("Open " + source_.name);
  // Microsoft 365 has no app of its own to open, only Word, Excel and the rest.
  banner_primary_->setVisible(launcher_installed_ && id_ != "office");
  import_button_->setVisible(launcher_installed_);
  // Microsoft 365's apps are listed once it is set up.
  if (id_ == "office" && launcher_installed_ && !was_installed) RefreshOwned();
  UpdateStatusLine();
}

void SourcePage::UpdateStatusLine() {
  const theme::Tokens& tokens = theme::Current();
  QStringList parts;
  if (IsStore()) {
    if (tool_updating_) {
      parts << StatusDot(tokens.info) + "Updating " + CopyFor(id_).tool + "…";
    } else if (!tool_installed_) {
      parts << StatusDot(tokens.warning) + "Not set up";
    } else if (!authenticated_) {
      parts << StatusDot(tokens.warning) + "Not signed in";
    } else {
      parts << StatusDot(tokens.success) +
                   (account_.empty() ? QString("Signed in") : "Signed in as " + QString::fromStdString(account_).toHtmlEscaped());
    }
  } else if (IsLauncher()) {
    parts << (launcher_installing_ ? StatusDot(tokens.info) + "Installing…"
              : launcher_installed_ ? StatusDot(tokens.success) + "Installed"
                                    : StatusDot(tokens.warning) + "Not installed");
  }
  if (id_ != "humble") {
    const QString item = CopyFor(id_).item;
    parts << (library_count_ == 1 ? QString("1 %1 in your library").arg(item)
                                  : QString("%1 %2s in your library").arg(library_count_).arg(item));
  }
  status_line_->setText(parts.join("  ·  "));

  if (library_empty_ == nullptr) return;
  library_empty_->setVisible(library_count_ == 0);
  library_grid_->setVisible(library_count_ > 0);
  if (IsStore() && !authenticated_) {
    library_empty_->setText("Games from " + source_.name + " show up here once you're signed in.");
  } else if (IsLauncher() && !launcher_installed_) {
    library_empty_->setText("Games you install through " + source_.name + " show up here.");
  } else if (id_ == "local") {
    library_empty_->setText("Games you add yourself, or that a scan finds in your library folders, show up here.");
  } else if (HasImport()) {
    library_empty_->setText("Nothing from " + source_.name + " in your library yet. \"" +
                            CopyFor(id_).import_button + "\" brings in what's already installed.");
  }
}

void SourcePage::Import() {
  import_button_->setEnabled(false);
  import_result_->setVisible(false);
  // The whole ApiError, so the line keeps mirad's hint.
  const auto done = [this](bool ok, const ApiError& error, int added, int updated) {
    import_button_->setEnabled(true);
    if (ok) {
      ShowLine(import_result_, ImportOutcome(added, updated), "muted");
    } else {
      ShowError(import_result_, "Could not import.", error);
    }
    if (ok && (added > 0 || updated > 0)) emit LibraryChanged();
  };
  if (id_ == "local") {
    api::ScanLibraryAsync(this, [done](ScanResult r) { done(r.ok, r.error, r.added, 0); });
  } else if (id_ == "steam") {
    api::ScanSteamAsync(this, [done](SteamScanResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else if (id_ == "lutris") {
    api::ImportLutrisAsync(this, [done](LutrisImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else if (IsLauncher()) {
    api::ImportLauncherAsync(this, id_,
                                     [done](StoreImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else {
    api::ImportStoreAsync(this, id_,
                                  [done](StoreImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  }
}

void SourcePage::RefreshOwned(bool fresh) {
  owned_refresh_->setEnabled(false);
  owned_loading_ = true;
  const int generation = ++owned_generation_;
  // A stored list answers within a frame or two; only a store actually being asked is worth showing.
  QTimer::singleShot(150, this, [this, generation] {
    if (generation != owned_generation_ || !owned_loading_) return;
    owned_waiting_ = true;
    UpdateOwnedLoading();
  });
  const auto done = [this, generation] {
    if (generation != owned_generation_) return false;
    owned_loading_ = false;
    owned_waiting_ = false;
    owned_loaded_ = true;
    owned_refresh_->setEnabled(owned_available_);
    return true;
  };
  if (id_ == "humble") {
    api::GetHumbleLibraryAsync(this, [this, done](HumbleLibraryResult r) {
      if (!done()) return;
      ShowBundles(r);
      UpdateOwnedLoading();
    });
  } else {
    api::GetStoreLibraryAsync(this, id_, fresh, [this, done](StoreLibraryResult r) {
      if (!done()) return;
      ShowOwned(r);
      UpdateOwnedLoading();
    });
  }
}

void SourcePage::UpdateOwnedLoading() {
  if (owned_rail_ == nullptr) return;
  // The first time a store is asked there's nothing to show yet; after that the list stays put.
  const bool first = owned_waiting_ && owned_.empty() && !owned_loaded_;
  const bool quiet = !first && (owned_waiting_ || checking_);
  owned_skeleton_->setVisible(first);
  owned_rail_->SetFaint(!first);
  owned_rail_->SetProgress(first || quiet ? -1 : 0);
  owned_rail_->setVisible(first || quiet);
  UpdateOwnedCount();
}

void SourcePage::UpdateOwnedCount() {
  if (owned_count_ == nullptr) return;
  if (owned_waiting_ && owned_.empty() && !owned_loaded_) {
    owned_count_->setText("Asking " + source_.name + "…");
    clear_filters_->setVisible(false);
    return;
  }
  const int total = static_cast<int>(owned_.size());
  const int shown = owned_grid_ != nullptr ? owned_grid_->VisibleCount() : total;
  owned_count_->setText(total == 0 ? QString() : shown == total ? QString::number(total)
                                                                : QString("%1 of %2").arg(shown).arg(total));
  clear_filters_->setVisible(chips_ != nullptr && !chips_->Filter().Empty());
}

void SourcePage::ShowOwned(const StoreLibraryResult& result) {
  // Signed out since it was asked: the sign-in line stays.
  if (status_known_ && !owned_available_) return;
  owned_.clear();
  not_owned_.clear();
  tiers_.clear();
  tags_.clear();
  reviews_.clear();
  review_percents_.clear();
  if (steam_settings_ != nullptr) steam_settings_->setVisible(false);
  if (!result.ok) {
    ShowError(owned_note_, "Could not list your games.", result.error);
    RebuildOwnedTiles();
    return;
  }
  std::vector<StoreTitle> uninstalled;
  for (const StoreTitle& title : result.titles) {
    downloads_->NoteTitle(source_.id, QString::fromStdString(title.ref), QString::fromStdString(title.title));
    if (!title.installed) {
      owned_.emplace_back(QString::fromStdString(title.ref), QString::fromStdString(title.title));
      if (!title.owned) not_owned_.insert(QString::fromStdString(title.ref));
      if (!title.protondb_tier.empty()) {
        tiers_.insert(QString::fromStdString(title.ref), QString::fromStdString(title.protondb_tier));
      }
      if (!title.steam_tags.empty()) tags_.insert(QString::fromStdString(title.ref), ToQStringList(title.steam_tags));
      if (!title.review_summary.empty()) {
        reviews_.insert(QString::fromStdString(title.ref), QString::fromStdString(title.review_summary));
      }
      if (title.review_percent >= 0) review_percents_.insert(QString::fromStdString(title.ref), title.review_percent);
      uninstalled.push_back(title);
    }
  }
  // Covers already fetched are skipped; the rest arrive as events.
  if (!uninstalled.empty()) {
    api::QueueTitleArtworkAsync(this, id_, std::move(uninstalled), [](StoreActionResult) {});
  }
  if (result.titles.empty() && id_ == "steam") {
    ShowLine(owned_note_,
             "Set a Steam Web API key and your SteamID64 in Settings to see the games you own "
             "but haven't installed.",
             "muted");
    if (steam_settings_ != nullptr) steam_settings_->setVisible(true);
  } else if (owned_.empty()) {
    ShowLine(owned_note_, "Everything you own is installed.", "muted");
  } else {
    ShowLine(owned_note_,
             id_ == "steam" ? "Install hands the game to the Steam client; scan the Steam library "
                              "once it's done."
                            : QString(),
             "muted");
  }
  RebuildOwnedTiles();
}

void SourcePage::ShowBundles(const HumbleLibraryResult& result) {
  owned_.clear();
  if (!result.ok) {
    ShowError(owned_note_, "Could not list your purchases.", result.error);
    RebuildOwnedTiles();
    return;
  }
  for (const HumbleBundle& bundle : result.bundles) {
    owned_.emplace_back(QString::fromStdString(bundle.key), QString::fromStdString(bundle.name));
    downloads_->NoteTitle("humble", QString::fromStdString(bundle.key), QString::fromStdString(bundle.name));
  }
  ShowLine(owned_note_,
           owned_.empty() ? QString("No purchases on this account.")
                          : QString("Downloads land in your games folder, where Mira picks up "
                                    "anything it recognizes."),
           "muted");
  RebuildOwnedTiles();
}

void SourcePage::RebuildOwnedTiles() {
  // The same titles in the same order (every progress tick): roles updated
  // in place, so the hover card and selection stay put.
  bool same = owned_model_->rowCount() == static_cast<int>(owned_.size());
  for (int row = 0; same && row < owned_model_->rowCount(); ++row) {
    same = owned_model_->item(row)->data(GameTileDelegate::IdRole).toString() == owned_[row].first;
  }
  if (!same) {
    ShowHoverCard(nullptr, QModelIndex());
    owned_model_->clear();
    for (const auto& [ref, title] : owned_) {
      auto* item = new QStandardItem();
      item->setData(ref, GameTileDelegate::IdRole);
      item->setData(title, GameTileDelegate::NameRole);
      item->setData(QString("ready"), GameTileDelegate::StatusRole);
      owned_model_->appendRow(item);
    }
  }
  const QString idle = id_ == "humble" ? "Download" : "Install";
  for (int row = 0; row < owned_model_->rowCount(); ++row) {
    QStandardItem* item = owned_model_->item(row);
    const auto& [ref, title] = owned_[row];
    // Bundles aren't games, so there's no cover to look up.
    item->setData(id_ == "humble" ? PlaceholderCover(title, source_.id + "-" + ref, tile_, devicePixelRatioF())
                                  : artwork_->TitleCover(source_.id, ref, title, tile_, devicePixelRatioF()),
                  Qt::DecorationRole);
    item->setData(tiers_.value(ref), GameTileDelegate::ProtonDbRole);
    item->setData(review_percents_.contains(ref) ? QVariant(review_percents_.value(ref)) : QVariant(),
                  GameTileDelegate::ReviewRole);
    // Cleared once the install stops: the item is reused.
    const QString state = owned_state_.value(ref);
    std::optional<DownloadTracker::TileProgress> installing;
    const DownloadTracker::Entry* running = downloads_->Find(source_.id + ":" + ref);
    if (running != nullptr && running->state == DownloadTracker::State::Running) {
      installing = DownloadTracker::TileProgressFor(*running);
    } else if (state.endsWith("…")) {  // asked for, before mirad's first event
      installing = DownloadTracker::TileProgress{.fraction = -1, .status = state, .detail = {}};
    }
    if (not_owned_.contains(ref)) {
      GameTileDelegate::SetTileProgress(*item, std::nullopt, "Not owned");
      item->setData(false, GameTileDelegate::ActionEnabledRole);
      item->setToolTip(title + "\nA paid game from a collection. Buy it on itch.io to install it here.");
      continue;
    }
    const QString action = humble_paths_.contains(ref) ? QString("Add to library…") : idle;
    GameTileDelegate::SetTileProgress(*item, installing, installing || state.isEmpty() ? action : state);
    // An outcome ("Sent to Steam") shows on the pill until the list refreshes.
    if (!installing && !state.isEmpty()) item->setData(false, GameTileDelegate::ActionEnabledRole);
  }
  if (chips_ != nullptr) {
    std::vector<RatingChips::Title> rated;
    rated.reserve(owned_.size());
    for (const auto& [ref, title] : owned_) {
      rated.push_back({tiers_.value(ref).toStdString(), reviews_.value(ref).toStdString()});
    }
    chips_->SetTitles(rated);
    chips_->setVisible(!owned_.empty());
  }
  ApplyFilter();
}

void SourcePage::StartInstall(const QString& ref, bool update) {
  owned_state_.insert(ref, update ? "Updating…" : "Installing…");
  if (owned_grid_ != nullptr) RebuildOwnedTiles();
  api::InstallStoreTitleAsync(this, id_, ref.toStdString(), update,
                                      [this, ref, update](StoreActionResult r) {
                                        if (r.ok) return;  // events report the rest
                                        owned_state_.remove(ref);
                                        if (owned_grid_ != nullptr) RebuildOwnedTiles();
                                        // An update starts from "In your library", an install from below.
                                        ShowError(update || owned_note_ == nullptr ? import_result_ : owned_note_,
                                                  "Could not start it.", r.error);
                                      });
}

void SourcePage::ApplyFilter() {
  const QString needle = filter_->text().trimmed();
  if (library_grid_ != nullptr) {
    games_->SetSearch(needle);
    library_grid_->FitHeight();
  }
  if (owned_grid_ != nullptr) {
    for (int row = 0; row < owned_model_->rowCount(); ++row) {
      const QStandardItem* item = owned_model_->item(row);
      const QString ref = item->data(GameTileDelegate::IdRole).toString();
      // A title's Steam tags match whole, so "rpg" finds RPGs without matching every word containing it.
      const bool found = needle.isEmpty() ||
                         item->data(GameTileDelegate::NameRole).toString().contains(needle, Qt::CaseInsensitive) ||
                         tags_.value(ref).contains(needle, Qt::CaseInsensitive);
      const bool rated = chips_ == nullptr ||
                         chips_->Filter().Matches(id_, tiers_.value(ref).toStdString(), reviews_.value(ref).toStdString());
      owned_grid_->setRowHidden(row, !found || !rated);
    }
    owned_grid_->FitHeight();
  }
  UpdateOwnedCount();
}

void SourcePage::ShowHoverCard(TileGrid* grid, const QModelIndex& index) {
  if (!index.isValid()) {
    if (hover_card_ != nullptr) hover_card_->hide();
    return;
  }
  if (hover_card_ == nullptr) hover_card_ = new HoverCard(this);
  if (grid == library_grid_) {
    const GameSummary* game = games_->GameAt(index);
    if (game == nullptr) return;
    hover_card_->ShowGame(*game, game->running);
  } else {
    // A running install's status line, else the pill's outcome; an idle one just says Install.
    const QString ref = index.data(GameTileDelegate::IdRole).toString();
    QString status = index.data(GameTileDelegate::StatusTextRole).toString();
    if (status.isEmpty()) status = index.data(GameTileDelegate::ActionRole).toString();
    if (humble_paths_.contains(ref)) {
      status = "Downloaded to " + humble_paths_.value(ref);
    } else if (index.data(GameTileDelegate::ActionEnabledRole).toBool()) {
      status = id_ == "humble" ? "Not downloaded" : "Not installed";
    }
    hover_card_->ShowTitle(QString::fromStdString(id_), ref, index.data(GameTileDelegate::NameRole).toString(), status,
                           source_.name);
  }
  const QRect tile = grid->visualRect(index);
  hover_card_->PopUpBeside(QRect(grid->viewport()->mapToGlobal(tile.topLeft()), tile.size()));
}

void SourcePage::ShowTileNote(const QString& id, const QString& text) {
  if (library_grid_ != nullptr) GameTileDelegate::ShowNote(library_grid_, id, text);
}

void SourcePage::SetDragSelectEnabled(bool enabled) {
  for (TileGrid* grid : {library_grid_, owned_grid_}) {
    if (grid != nullptr) grid->SetDragSelectEnabled(enabled);
  }
}

// Right-clicking outside the selection replaces it, as in the library grid.
QModelIndexList SelectForMenu(TileGrid* grid, const QModelIndex& index) {
  if (!grid->selectionModel()->isSelected(index)) {
    grid->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect);
  }
  return grid->selectionModel()->selectedIndexes();
}

void SourcePage::ShowLibraryMenu(const QPoint& pos) {
  const QModelIndex index = library_grid_->indexAt(pos);
  if (!index.isValid()) return;
  if (const QModelIndexList selected = SelectForMenu(library_grid_, index); selected.size() > 1) {
    QStringList ids;
    for (const QModelIndex& it : selected) ids << it.data(GameTileDelegate::IdRole).toString();
    emit BatchMenuRequested(ids, library_grid_->viewport()->mapToGlobal(pos));
    return;
  }
  const QString id = index.data(GameTileDelegate::IdRole).toString();
  // A store game's id is "<source>-<ref>"; those can also be updated.
  const QString prefix = source_.id + "-";
  const QString update_ref = IsStore() && id_ != "humble" && id.startsWith(prefix) ? id.mid(prefix.size()) : QString();
  emit GameMenuRequested(id, library_grid_->viewport()->mapToGlobal(pos), update_ref);
}

void SourcePage::ShowOwnedMenu(const QPoint& pos) {
  const QModelIndex index = owned_grid_->indexAt(pos);
  if (!index.isValid()) return;
  QStringList refs;
  for (const QModelIndex& it : SelectForMenu(owned_grid_, index)) {
    if (it.data(GameTileDelegate::ActionEnabledRole).toBool()) refs << it.data(GameTileDelegate::IdRole).toString();
  }
  // One downloaded bundle adds to the library; several only download the rest.
  const bool add = refs.size() == 1 && humble_paths_.contains(refs.front());
  if (!add) refs.removeIf([this](const QString& ref) { return humble_paths_.contains(ref); });
  QMenu menu(this);
  const QString verb = add ? "Add to library…" : id_ == "humble" ? "Download" : "Install";
  QAction* start = menu.addAction(refs.size() > 1 ? QString("%1 (%2)").arg(verb).arg(refs.size()) : verb);
  start->setEnabled(!refs.isEmpty());
  if (menu.exec(owned_grid_->viewport()->mapToGlobal(pos)) != start || !owned_grid_->on_action) return;
  // Looked up again per title: each action can rebuild the tiles.
  for (const QString& ref : refs) {
    for (int row = 0; row < owned_model_->rowCount(); ++row) {
      const QModelIndex match = owned_model_->index(row, 0);
      if (match.data(GameTileDelegate::IdRole).toString() != ref) continue;
      owned_grid_->on_action(match);
      break;
    }
  }
}

void SourcePage::UpdateTitle(const QString& ref) { StartInstall(ref, /*update=*/true); }

void SourcePage::HandleEvent(const std::string& type, const std::string& data) {
  if (StoreEvent art; events::ParseTitleArtworkEvent(type, data, &art)) {
    // "ready" is LibraryWindow's: it has to land while this page is closed too.
    if (art.source == id_ && art.state == "failed" && art.error.code == "no_steamgriddb_key" &&
        art.error.fix.kind == "setting" && art_key_ != nullptr) {
      art_key_setting_ = QString::fromStdString(art.error.fix.target);
      art_key_->setToolTip(error_help::HintFor(art.error));
      art_key_->setVisible(true);
    }
    return;
  }

  if (CatalogCheckEvent check; events::ParseCatalogCheck(type, data, &check)) {
    if (check.source != id_ || owned_section_ == nullptr) return;
    checking_ = check.checking;
    UpdateOwnedLoading();
    if (check.changed) RefreshOwned();  // a list asked for before it changed is dropped
    return;
  }

  StoreEvent event;
  if (!events::ParseStoreEvent(type, data, &event) || event.source != id_) return;

  if (event.kind == "setup") {
    if (event.state == "finished") {
      launcher_installing_ = false;
      tool_updating_ = false;
      RefreshStatus();
    } else if (event.state == "failed") {
      launcher_installing_ = false;
      tool_updating_ = false;
      if (event.error.code == "cancelled") {
        RefreshStatus();
      } else {
        setup_card_->ShowSetupFailed(event.error, IsStore() && tool_installed_);
      }
      UpdateStatusLine();
    }
    return;
  }

  const QString ref = QString::fromStdString(event.ref);
  if (event.state == "failed") {
    owned_state_.remove(ref);
    if (event.error.code != "cancelled") {
      ShowError(owned_note_ != nullptr ? owned_note_ : import_result_, "It failed.", event.error);
    }
  } else if (event.state == "finished") {
    if (id_ == "steam") {
      owned_state_.insert(ref, "Sent to Steam");
    } else {
      // Now a tracked game: it moves to "In your library" on the next relist.
      owned_state_.remove(ref);
      std::erase_if(owned_, [&ref](const auto& entry) { return entry.first == ref; });
    }
  } else {
    return;
  }
  if (owned_grid_ != nullptr) RebuildOwnedTiles();
}

}  // namespace mira_gui
