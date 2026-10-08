#include "LibraryPage.h"

#include <QCursor>
#include <QItemSelectionModel>
#include <QResizeEvent>
#include <QTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <map>

#include "../theme/Theme.h"
#include "../widgets/TabRow.h"
#include "../widgets/TileView.h"
#include "ArtworkStore.h"
#include "ContinueRow.h"
#include "FilterSortPill.h"
#include "GameLibraryModel.h"
#include "GameTileDelegate.h"
#include "TileGrid.h"
#include "../client/api/Stores.h"
#include "../sources/Sources.h"

namespace mira_gui {

// setViewportMargins is protected on QAbstractScrollArea; this republishes
// it so ApplyLayoutTokens() can pad the tiles without also insetting the
// scrollbar. Ctrl+wheel resizes tiles instead of scrolling.
class LibraryGrid : public TileView {
 public:
  using TileView::setViewportMargins;
  using TileView::TileView;

  // One call per notch, positive to grow.
  std::function<void(int steps)> on_ctrl_wheel;
  // Sized to its tiles inside the page's scroll (see LibraryPage::FitGrid), so the wheel is the page's.
  bool page_scrolls = false;

 protected:
  void wheelEvent(QWheelEvent* event) override {
    if (event->modifiers() & Qt::ControlModifier) {
      // angleDelta() is in eighths of a degree; a notch is 15 degrees (120).
      const int steps = event->angleDelta().y() / 120;
      if (steps != 0 && on_ctrl_wheel) on_ctrl_wheel(steps);
      event->accept();
      return;
    }
    if (page_scrolls) {
      StopHover();
      event->ignore();
      return;
    }
    TileView::wheelEvent(event);
  }
};

namespace {

// The filters the tab row offers, with its own shorter labels.
const std::pair<const char*, const char*> kFilterTabs[] = {
    {"all", "All"},
    {"games", "Games"},
    {"apps", "Apps"},
    {"ready", "Installed"},
    {"running", "Playing now"},
    {"attention", "Needs attention"},
    {"never", "Never played"},
};

}  // namespace

LibraryPage::LibraryPage(GameLibraryModel* library, ArtworkStore* artwork,
                         const std::string& sort_key, bool sort_descending, int tile_width,
                         QWidget* parent)
    : QWidget(parent), library_(library), artwork_(artwork), tile_width_(tile_width) {
  games_ = new GameFilterProxy(library_, this);
  games_->SetSort(sort_key, sort_descending);

  auto* layout = new QVBoxLayout(this);
  // Plus the grid's own padding and tile inset, lines up with the header.
  layout->setContentsMargins(11, 0, 11, 0);
  layout->setSpacing(6);
  layout->addWidget(BuildHeader(sort_key, sort_descending));

  grid_ = new LibraryGrid(this);
  grid_->setObjectName("library_grid");
  delegate_ = new GameTileDelegate(grid_, TileSize(), artwork_);
  grid_->setItemDelegate(delegate_);
  grid_->setModel(games_);
  grid_->setViewMode(QListView::IconMode);
  grid_->setResizeMode(QListView::Adjust);
  grid_->setMovement(QListView::Static);
  grid_->setUniformItemSizes(true);
  grid_->setSpacing(0);
  grid_->setGridSize(TileSize());
  grid_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  grid_->setFrameShape(QFrame::NoFrame);
  grid_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  // Default QListView scrolling moves one item per wheel tick, a visible
  // jump for a 250px tile. Per-pixel smooths it; not also grabbing a
  // QScroller drag gesture, which would fight single-click-select.
  grid_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  grid_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(grid_, &QListView::customContextMenuRequested, this, &LibraryPage::ShowContextMenu);
  connect(grid_, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& index) {
    if (const GameSummary* game = games_->GameAt(index)) emit GameActivated(game->id);
  });
  connect(grid_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &LibraryPage::SelectionChanged);
  grid_->on_hover = [this](const QModelIndex& index) { Hover(index); };
  grid_->on_ctrl_wheel = [this](int steps) { emit ZoomStepped(steps); };
  ApplyLayoutTokens();
  // The padding around the tiles is background too; see eventFilter.
  grid_->installEventFilter(this);
  installEventFilter(this);

  // The grid fills this and scrolls itself, until a search also lists store
  // titles below it: then the grid fits its tiles and this scrolls both.
  scroll_ = new QScrollArea(this);
  scroll_->setObjectName("library_scroll");
  scroll_->setWidgetResizable(true);
  scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* content = new QWidget(scroll_);
  auto* content_layout = new QVBoxLayout(content);
  content_layout->setContentsMargins(0, 0, 0, 0);
  content_layout->setSpacing(6);
  content_layout->addWidget(grid_, /*stretch=*/1);
  empty_hint_ = new QLabel(content);
  empty_hint_->setAlignment(Qt::AlignCenter);
  empty_hint_->setProperty("role", "muted");
  empty_hint_->setVisible(false);
  content_layout->addWidget(empty_hint_);
  content_layout->addWidget(BuildOwnedSection());
  content_layout->addStretch(0);  // takes the spare height while the grid fits its tiles; see FitGrid
  content_layout_ = content_layout;
  scroll_->setWidget(content);
  scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);  // see FitGrid
  layout->addWidget(scroll_, /*stretch=*/1);

  connect(library_, &GameLibraryModel::Changed, this, &LibraryPage::LibraryChanged);
  // The tiles are painted, so the stylesheet can't restyle them.
  connect(theme::Notifier::Instance(), &theme::Notifier::Changed, this, [this] {
    ApplyLayoutTokens();
    grid_->viewport()->update();
  });
}

QWidget* LibraryPage::BuildHeader(const std::string& sort_key, bool sort_descending) {
  auto* top = new QWidget(this);
  auto* layout = new QVBoxLayout(top);
  layout->setContentsMargins(11, 14, 11, 0);
  layout->setSpacing(14);

  tabs_ = new TabRow(top);
  for (const auto& [key, label] : kFilterTabs) tabs_->AddTab(key, label);
  tabs_->SetAlert("attention", true);
  tabs_->SetCurrent("all");
  connect(tabs_, &TabRow::CurrentChanged, this, &LibraryPage::SetFilterKey);

  pill_ = new FilterSortPill(sort_key, sort_descending, top);
  connect(pill_, &FilterSortPill::FilterChanged, this, [this] {
    tabs_->SetCurrent(FilterKey());
    ApplyFilter();
    emit FilterChanged();
  });
  connect(pill_, &FilterSortPill::SortChanged, this, [this] {
    games_->SetSort(pill_->SortKey(), pill_->SortDescending());
    grid_->scrollToTop();
    emit SortChanged();
  });
  connect(pill_, &FilterSortPill::TagsChanged, this, &LibraryPage::ApplyFilter);
  tabs_->SetTrailing(pill_);
  search_ = new QLineEdit(top);
  search_->setObjectName("library_search");
  search_->setPlaceholderText("Search…");
  search_->setClearButtonEnabled(true);
  connect(search_, &QLineEdit::textChanged, this, &LibraryPage::ApplyFilter);
  tabs_->SetSearch(search_);
  layout->addWidget(tabs_);

  continue_row_ = new ContinueRow(artwork_, top);
  continue_row_->setVisible(false);
  connect(continue_row_, &ContinueRow::PlayToggled, this,
          [this](const QString& id) { emit PlayRequested(id.toStdString()); });
  connect(continue_row_, &ContinueRow::MenuRequested, this,
          [this](const QString& id, const QPoint& pos) {
            emit GameMenuRequested(id.toStdString(), pos);
          });
  layout->addWidget(continue_row_);
  return top;
}

void LibraryPage::ApplyPrefs(const FrontendPrefs& prefs) {
  tabs_->SetTabsVisible(prefs.library_filter_tabs.value_or(true));
  continue_row_enabled_ = prefs.library_continue_row.value_or(true);
  continue_count_ = prefs.library_continue_count.value_or(3);
  continue_apps_ = prefs.library_continue_apps.value_or(false);
  apps_in_all_ = prefs.library_apps_in_all.value_or(true);
  games_->SetAppsInAll(apps_in_all_);
  UpdateCounts();  // All's count follows apps_in_all_
  delegate_->SetShowStatus(prefs.tile_status.value_or(true));
  delegate_->SetShowSourceMark(prefs.tile_source_mark.value_or(true));
  delegate_->SetShowPinBadge(prefs.tile_pin_badge.value_or(true));
  grid_->SetDragSelectEnabled(prefs.drag_select.value_or(true));
  ApplyFilter();
}

QString LibraryPage::FilterKey() const {
  return pill_->FilterKey();
}

void LibraryPage::SetFilterKey(const QString& key) {
  if (const int row = pill_->FilterRow(key); row >= 0) pill_->SetFilterRow(row);
}

void LibraryPage::PickFilter(int row) {
  pill_->SetFilterRow(row);
}

int LibraryPage::FilterCount() const {
  return static_cast<int>(pill_->FilterKeys().size());
}

void LibraryPage::ToggleHidden() {
  SetFilterKey(FilterKey() == "hidden" ? "all" : "hidden");
}

const std::string& LibraryPage::SortKey() const {
  return pill_->SortKey();
}

bool LibraryPage::SortDescending() const {
  return pill_->SortDescending();
}

void LibraryPage::FocusSearch() {
  tabs_->OpenSearch();  // a narrow window shows only its button
  search_->setFocus(Qt::ShortcutFocusReason);
  search_->selectAll();
}

void LibraryPage::ClearSearchOrSelection() {
  if (!search_->text().isEmpty()) {
    search_->clear();
    return;
  }
  ClearSelection();
}

void LibraryPage::ClearSelection() {
  grid_->clearSelection();
  grid_->setCurrentIndex(QModelIndex());
}

std::vector<std::pair<std::string, QString>> LibraryPage::SelectedGames() const {
  // In grid order, not the order they were picked in.
  QModelIndexList rows = grid_->selectionModel()->selectedIndexes();
  std::ranges::sort(rows,
                    [](const QModelIndex& a, const QModelIndex& b) { return a.row() < b.row(); });
  std::vector<std::pair<std::string, QString>> games;
  for (const QModelIndex& index : rows) {
    games.emplace_back(index.data(GameTileDelegate::IdRole).toString().toStdString(),
                       index.data(GameTileDelegate::NameRole).toString());
  }
  return games;
}

std::string LibraryPage::SelectedId() const {
  const QModelIndexList rows = grid_->selectionModel()->selectedIndexes();
  return rows.size() == 1 ? rows.front().data(GameTileDelegate::IdRole).toString().toStdString()
                          : std::string();
}

void LibraryPage::ShowTileNote(const std::string& id, const QString& text) {
  GameTileDelegate::ShowNote(grid_, QString::fromStdString(id), text);
}

bool LibraryPage::ToggleSelected(const std::string& id) {
  const QModelIndex tile = games_->mapFromSource(library_->IndexOf(id));
  if (!tile.isValid()) return false;
  grid_->selectionModel()->select(tile, QItemSelectionModel::Toggle);
  return true;
}

bool LibraryPage::ShowGame(const std::string& id) {
  const QModelIndex tile = games_->mapFromSource(library_->IndexOf(id));
  if (!tile.isValid()) return false;
  grid_->setCurrentIndex(tile);  // ClearAndSelect: this game alone
  grid_->scrollTo(tile, QAbstractItemView::PositionAtCenter);
  return true;
}

int LibraryPage::ShownCount() const {
  return games_->rowCount();
}

QSize LibraryPage::TileSize() const {
  // 2:3 portrait cover ratio, plus room for the title band over the bottom.
  return QSize(tile_width_, tile_width_ * 3 / 2);
}

void LibraryPage::SetTileWidth(int width) {
  if (width == tile_width_) return;
  tile_width_ = width;
  // Each step scales every visible cover again, so they're scaled quickly until the zoom stops.
  if (zoom_settled_ == nullptr) {
    zoom_settled_ = new QTimer(this);
    zoom_settled_->setSingleShot(true);
    zoom_settled_->setInterval(150);
    connect(zoom_settled_, &QTimer::timeout, this, [this] {
      artwork_->SetQuickScaling(false);
      // The covers kept at the old size are no longer drawn.
      if (drawn_width_ != 0 && drawn_width_ != tile_width_) artwork_->ForgetWidth(drawn_width_);
      drawn_width_ = tile_width_;
      grid_->viewport()->update();
      if (!owned_matches_.empty()) UpdateOwnedMatches();
    });
  }
  artwork_->SetQuickScaling(true);
  zoom_settled_->start();
  delegate_->SetTileSize(TileSize());
  grid_->setGridSize(TileSize());
  owned_grid_->SetTileSize(TileSize());
  if (!owned_matches_.empty()) UpdateOwnedMatches();  // covers at the new size
  FitGrid();
}

void LibraryPage::UpdateCover(const std::string& id) {
  continue_row_->RefreshCover(id);
}

void LibraryPage::SetControlsEnabled(bool enabled) {
  // Disabling the pill blocks its popover too.
  pill_->setEnabled(enabled);
  search_->setEnabled(enabled);
}

QWidget* LibraryPage::ShortcutScope() const {
  return grid_;
}

bool LibraryPage::eventFilter(QObject* watched, QEvent* event) {
  // A new width wraps the tiles into a different number of rows.
  if (watched == grid_ && event->type() == QEvent::Resize && grid_->page_scrolls) {
    const auto* resize = static_cast<QResizeEvent*>(event);
    if (resize->size().width() != resize->oldSize().width()) QTimer::singleShot(0, this, &LibraryPage::FitGrid);
  }
  // Clicks in the grid's own padding (outside its viewport) or around it deselect.
  if ((watched == grid_ || watched == this) && event->type() == QEvent::MouseButtonPress &&
      static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
    ClearSelection();
  }
  return QWidget::eventFilter(watched, event);
}

QWidget* LibraryPage::BuildOwnedSection() {
  owned_section_ = new QWidget(this);
  owned_section_->setVisible(false);
  auto* layout = new QVBoxLayout(owned_section_);
  // The grid's own viewport margins, so the heading lines up with the tiles above.
  const int margin = theme::Current().grid_margin;
  layout->setContentsMargins(margin, 8, margin, margin);
  layout->setSpacing(6);
  owned_heading_ = new QLabel(owned_section_);
  owned_heading_->setProperty("role", "heading");
  layout->addWidget(owned_heading_);

  owned_model_ = new QStandardItemModel(this);
  owned_grid_ = new TileGrid(TileSize(), artwork_, owned_section_);
  owned_grid_->setModel(owned_model_);
  owned_grid_->setSelectionMode(QAbstractItemView::NoSelection);
  owned_grid_->on_action = [this](const QModelIndex& index) {
    InstallMatch(index, owned_grid_->viewport()->mapToGlobal(owned_grid_->visualRect(index).topRight()));
  };
  owned_grid_->on_ctrl_wheel = [this](int steps) { emit ZoomStepped(steps); };
  owned_grid_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(owned_grid_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
    const QModelIndex index = owned_grid_->indexAt(pos);
    if (index.isValid()) InstallMatch(index, owned_grid_->viewport()->mapToGlobal(pos));
  });
  connect(owned_grid_, &QAbstractItemView::doubleClicked, this,
          [this](const QModelIndex& index) { InstallMatch(index, QCursor::pos()); });
  layout->addWidget(owned_grid_);

  // A store title's cover arriving after the search drew its placeholder.
  connect(artwork_, &ArtworkStore::CoverChanged, this, [this](const QString& id) {
    for (int row = 0; row < owned_model_->rowCount(); ++row) {
      QStandardItem* item = owned_model_->item(row);
      if (item->data(GameTileDelegate::IdRole).toString() != id) continue;
      const auto& [source, ref] = owned_matches_[row].copies.front();
      item->setData(artwork_->TitleCover(source, ref, owned_matches_[row].title, TileSize(), devicePixelRatioF()),
                    Qt::DecorationRole);
    }
  });
  return owned_section_;
}

void LibraryPage::SetOwnedTitles(OwnedTitles* titles) {
  owned_titles_ = titles;
  connect(titles, &OwnedTitles::Changed, this, &LibraryPage::UpdateOwnedMatches);
  // Kept current ahead of the first search, so typing never waits on a store.
  connect(search_, &QLineEdit::textEdited, titles, &OwnedTitles::RefreshIfStale);
}

void LibraryPage::UpdateOwnedMatches() {
  const QString query = search_->text().trimmed();
  owned_matches_ = owned_titles_ != nullptr ? MatchOwned(owned_titles_->Titles(), query) : std::vector<OwnedMatch>{};
  owned_model_->clear();
  std::map<std::string, std::vector<StoreTitle>> art_to_fetch;
  for (const OwnedMatch& match : owned_matches_) {
    const auto& [source, ref] = match.copies.front();
    auto* item = new QStandardItem();
    item->setData(source + "-" + ref, GameTileDelegate::IdRole);
    item->setData(match.title, GameTileDelegate::NameRole);
    item->setData(QString("ready"), GameTileDelegate::StatusRole);
    item->setData(source, GameTileDelegate::SourceRole);
    item->setData(artwork_->TitleCover(source, ref, match.title, TileSize(), devicePixelRatioF()), Qt::DecorationRole);
    QStringList stores;
    for (const auto& [store, store_ref] : match.copies) {
      const SourceInfo* info = FindSourceInfo(store);
      stores << (info != nullptr ? info->name : store);
    }
    item->setData(stores.join(", "), GameTileDelegate::StatusTextRole);
    item->setToolTip(match.title + "\n" + stores.join(", "));
    owned_model_->appendRow(item);
    if (covers_asked_.insert(source + "-" + ref).second) {
      art_to_fetch[source.toStdString()].push_back({.ref = ref.toStdString(), .title = match.title.toStdString(),
                                                    .installed = false, .owned = true, .source = source.toStdString()});
    }
  }
  // Covers come from each store, once per title, only for what a search showed.
  for (auto& [source, titles] : art_to_fetch) api::QueueTitleArtworkAsync(this, source, std::move(titles), {});
  RefreshOwnedStates();
  owned_heading_->setText(QString("Not installed  %1").arg(owned_matches_.size()));
  owned_section_->setVisible(!owned_matches_.empty());
  UpdateEmptyState();
  FitGrid();
}

void LibraryPage::RefreshOwnedStates() {
  for (int row = 0; row < owned_model_->rowCount() && row < static_cast<int>(owned_matches_.size()); ++row) {
    std::optional<DownloadTracker::TileProgress> installing;
    for (const auto& [source, ref] : owned_matches_[row].copies) {
      if (title_progress) installing = title_progress(source, ref);
      if (installing) break;
    }
    QStandardItem* item = owned_model_->item(row);
    GameTileDelegate::SetTileProgress(*item, installing, "Install");
  }
}

void LibraryPage::InstallMatch(const QModelIndex& index, const QPoint& global_pos) {
  if (!index.isValid() || index.row() >= static_cast<int>(owned_matches_.size())) return;
  if (!index.data(GameTileDelegate::ActionEnabledRole).toBool()) return;  // already installing
  const OwnedMatch& match = owned_matches_[index.row()];
  if (match.copies.size() == 1) {
    emit InstallTitleRequested(match.copies.front().first, match.copies.front().second);
    return;
  }
  // Owned on several stores: which copy to install is the player's call.
  QMenu menu(this);
  for (const auto& [source, ref] : match.copies) {
    const SourceInfo* info = FindSourceInfo(source);
    connect(menu.addAction("Install from " + (info != nullptr ? info->name : source)), &QAction::triggered, this,
            [this, source, ref] { emit InstallTitleRequested(source, ref); });
  }
  menu.exec(global_pos);
}

void LibraryPage::FitGrid() {
  const bool page_scrolls = owned_section_->isVisible();
  grid_->page_scrolls = page_scrolls;
  // The grid fills the page, or the results sit at the top with the space left below them.
  content_layout_->setStretchFactor(grid_, page_scrolls ? 0 : 1);
  content_layout_->setStretch(content_layout_->count() - 1, page_scrolls ? 1 : 0);
  if (!page_scrolls) {
    grid_->setVisible(true);
    grid_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    grid_->setMinimumHeight(0);
    grid_->setMaximumHeight(QWIDGETSIZE_MAX);
    scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return;
  }
  scroll_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  grid_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  grid_->setVisible(games_->rowCount() > 0);
  const QSize cell = grid_->gridSize();
  const int margin = theme::Current().grid_margin;
  const int per_row = std::max(1, (grid_->width() - 2 * margin) / std::max(1, cell.width()));
  const int rows = (games_->rowCount() + per_row - 1) / per_row;
  grid_->setFixedHeight(rows * cell.height() + 2 * margin + 2 * grid_->frameWidth());
}

void LibraryPage::ApplyLayoutTokens() {
  // Viewport margins, not the container's contents margins: those would
  // inset the whole frame, pushing its scrollbar in by the same amount.
  // This pads only the tiles' own drawing area, leaving the scrollbar
  // docked at the panel's true right edge.
  const int margin = theme::Current().grid_margin;
  grid_->setViewportMargins(margin, margin, margin, margin);
}

void LibraryPage::ApplyFilter() {
  // Rows the proxy drops leave the selection; nothing else moves.
  games_->SetFilterKey(FilterKey());
  games_->SetSearch(search_->text());
  games_->SetTags(pill_->PickedTags());
  grid_->scrollToTop();  // a new filter or search starts at the top
  scroll_->verticalScrollBar()->setValue(0);
  UpdateOwnedMatches();
  RefreshContinue();  // only shown under All, Games or Apps with no search
  UpdateCounts();     // they follow the picked tags, and the tags' counts the filter
  emit ShownChanged();
}

void LibraryPage::ShowTag(const QString& tag) {
  search_->clear();
  pill_->SetPickedTags({tag});
}

void LibraryPage::ClearFilters() {
  search_->clear();
  pill_->ClearFilters();
}

void LibraryPage::LibraryChanged() {
  UpdateCounts();
  UpdateEmptyState();
  FitGrid();
  RefreshContinue();
  emit ShownChanged();
}

void LibraryPage::UpdateCounts() {
  const QStringList picked = pill_->PickedTags();
  for (const QString& key : pill_->FilterKeys()) {
    const auto count = std::ranges::count_if(library_->Games(), [this, &key, &picked](const GameSummary& game) {
      return GameFilterProxy::MatchesKey(game, key, apps_in_all_) && GameFilterProxy::HasTags(game, picked);
    });
    pill_->SetCount(key, static_cast<int>(count));
    tabs_->SetCount(key, static_cast<int>(count));
  }
  QList<FilterSortPill::TagCount> tags;
  for (const LibraryTag& tag : TagsUnder(library_->Games(), FilterKey(), apps_in_all_)) {
    tags.append({tag.tag, tag.count});
  }
  pill_->SetTags(tags);
}

void LibraryPage::UpdateEmptyState() {
  const int shown = games_->rowCount();
  empty_hint_->setVisible(shown == 0);
  if (shown == 0) {
    const QString items = FilterKey() == "apps" ? "apps" : "games";
    empty_hint_->setText(library_->Games().empty() ? QString("No %1 in the library yet.").arg(items)
                         : !owned_matches_.empty() ? QString("Nothing installed matches.")
                                                   : QString("No %1 match this filter.").arg(items));
  }
}

void LibraryPage::RefreshContinue() {
  // Only over All, Games or Apps: under another filter or a search it's noise.
  std::vector<const GameSummary*> games;
  const QString key = FilterKey();
  if (continue_row_enabled_ && (key == "all" || key == "games" || (key == "apps" && continue_apps_)) &&
      search_->text().trimmed().isEmpty()) {
    for (const GameSummary& game : library_->Games()) {
      if (IsHidden(game) || game.source == "launcher" || !GameFilterProxy::MatchesKey(game, key, apps_in_all_)) continue;
      if (IsApp(game) && !continue_apps_) continue;
      if (game.running || game.last_played_at) games.push_back(&game);
    }
    const size_t keep = std::min(games.size(), static_cast<size_t>(continue_count_));
    std::partial_sort(games.begin(), games.begin() + keep, games.end(),
                      [](const GameSummary* a, const GameSummary* b) {
                        if (a->running != b->running) return a->running;
                        return a->last_played_at.value_or(0) > b->last_played_at.value_or(0);
                      });
    games.resize(keep);
  }
  continue_row_->SetGames(games);
}

void LibraryPage::ShowContextMenu(const QPoint& pos) {
  const QModelIndex index = grid_->indexAt(pos);
  if (!index.isValid()) return;
  QItemSelectionModel* selection = grid_->selectionModel();
  // Right-clicking outside the current selection replaces it, same as most
  // file managers; right-clicking inside a multi-selection keeps it so the
  // batch menu applies to everything that was selected.
  if (!selection->isSelected(index)) selection->select(index, QItemSelectionModel::ClearAndSelect);
  // NoUpdate: the default ClearAndSelect would drop a drag or Ctrl+A selection
  // whenever the right-clicked tile isn't already the current one.
  selection->setCurrentIndex(index, QItemSelectionModel::NoUpdate);

  const QPoint global = grid_->viewport()->mapToGlobal(pos);
  if (const QModelIndexList selected = selection->selectedIndexes(); selected.size() > 1) {
    std::vector<std::string> ids;
    for (const QModelIndex& row : selected) {
      ids.push_back(row.data(GameTileDelegate::IdRole).toString().toStdString());
    }
    emit BatchMenuRequested(ids, global);
    return;
  }
  emit GameMenuRequested(index.data(GameTileDelegate::IdRole).toString().toStdString(), global);
}

void LibraryPage::Hover(const QModelIndex& index) {
  if (!index.isValid()) {
    emit HoverEnded();
    return;
  }
  // A preview for one game reads as wrong noise over a multi-selection.
  if (grid_->selectionModel()->selectedIndexes().size() > 1) return;
  const GameSummary* game = games_->GameAt(index);
  if (game == nullptr) return;
  const QRect tile = grid_->visualRect(index);
  emit HoverRequested(game->id, QRect(grid_->viewport()->mapToGlobal(tile.topLeft()), tile.size()));
}

}  // namespace mira_gui
