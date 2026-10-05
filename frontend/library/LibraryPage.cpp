#include "LibraryPage.h"

#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>

#include "../theme/Theme.h"
#include "../widgets/TabRow.h"
#include "../widgets/TileView.h"
#include "ArtworkStore.h"
#include "ContinueRow.h"
#include "FilterSortPill.h"
#include "GameLibraryModel.h"
#include "GameTileDelegate.h"

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

 protected:
  void wheelEvent(QWheelEvent* event) override {
    if (event->modifiers() & Qt::ControlModifier) {
      // angleDelta() is in eighths of a degree; a notch is 15 degrees (120).
      const int steps = event->angleDelta().y() / 120;
      if (steps != 0 && on_ctrl_wheel) on_ctrl_wheel(steps);
      event->accept();
      return;
    }
    TileView::wheelEvent(event);
  }
};

namespace {

// The filters the tab row offers, with its own shorter labels.
const std::pair<const char*, const char*> kFilterTabs[] = {
    {"all", "All"},
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
  grid_->on_hover = [this](const QModelIndex& index) { Hover(index); };
  grid_->on_ctrl_wheel = [this](int steps) { emit ZoomStepped(steps); };
  layout->addWidget(grid_, /*stretch=*/1);
  ApplyLayoutTokens();
  // The padding around the tiles is background too; see eventFilter.
  grid_->installEventFilter(this);
  installEventFilter(this);

  empty_hint_ = new QLabel(this);
  empty_hint_->setAlignment(Qt::AlignCenter);
  empty_hint_->setProperty("role", "muted");
  empty_hint_->setVisible(false);
  layout->addWidget(empty_hint_);

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
  // Every size the slider passes through would otherwise stay cached until quit.
  artwork_->InvalidateAllRenderings();
  delegate_->SetTileSize(TileSize());
  grid_->setGridSize(TileSize());
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
  // Clicks in the grid's own padding (outside its viewport) or around it deselect.
  if ((watched == grid_ || watched == this) && event->type() == QEvent::MouseButtonPress &&
      static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
    ClearSelection();
  }
  return QWidget::eventFilter(watched, event);
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
  grid_->scrollToTop();  // a new filter or search starts at the top
  UpdateEmptyState();
  RefreshContinue();  // only shown under All with no search
  emit ShownChanged();
}

void LibraryPage::LibraryChanged() {
  UpdateCounts();
  UpdateEmptyState();
  RefreshContinue();
  emit ShownChanged();
}

void LibraryPage::UpdateCounts() {
  for (const QString& key : pill_->FilterKeys()) {
    const auto count = std::ranges::count_if(library_->Games(), [&key](const GameSummary& game) {
      return GameFilterProxy::MatchesKey(game, key);
    });
    pill_->SetCount(key, static_cast<int>(count));
    tabs_->SetCount(key, static_cast<int>(count));
  }
}

void LibraryPage::UpdateEmptyState() {
  const int shown = games_->rowCount();
  empty_hint_->setVisible(shown == 0);
  if (shown == 0) {
    empty_hint_->setText(library_->Games().empty() ? "No games in the library yet."
                                                   : "No games match this filter.");
  }
}

void LibraryPage::RefreshContinue() {
  // Only over the whole library: under a filter or a search it's noise.
  std::vector<const GameSummary*> games;
  if (continue_row_enabled_ && FilterKey() == "all" && search_->text().trimmed().isEmpty()) {
    for (const GameSummary& game : library_->Games()) {
      if (IsHidden(game) || IsApp(game) || game.source == "launcher") continue;
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
