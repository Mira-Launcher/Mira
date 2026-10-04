#include "LibraryWindow.h"
#include <algorithm>

#include <QAbstractItemView>
#include <QAction>
#include <QButtonGroup>
#include <QKeySequence>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QListWidgetItem>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QCloseEvent>
#include <QDrag>
#include <QMimeData>
#include <QApplication>
#include <QDropEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGraphicsDropShadowEffect>
#include <QGridLayout>
#include <QGuiApplication>
#include <QPushButton>
#include <QRubberBand>
#include <QScreen>
#include <QScrollBar>
#include <QItemSelection>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QSlider>
#include <QSplitter>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <functional>
#include <iterator>
#include <optional>

#include "../client/MiradClient.h"
#include "../dialogs/AddManualGameDialog.h"
#include "../dialogs/DesktopEntryImportDialog.h"
#include "../dialogs/GameDetailPageDialog.h"

#include "../ui/AboutPanel.h"
#include "../ui/CoverArt.h"
#include "../ui/DaemonSupervisor.h"
#include "../ui/DownloadTracker.h"
#include "../ui/DownloadsPanel.h"
#include "../client/EventHub.h"
#include "../ui/GameActions.h"
#include "../ui/GameEditForm.h"
#include "../ui/GamePresentation.h"
#include "../ui/GameLibraryModel.h"
#include "../ui/GameTileDelegate.h"
#include "../ui/ArtPickerPanel.h"
#include "../ui/HeroBackdrop.h"
#include "../ui/HoverCard.h"
#include "../ui/Icons.h"
#include "../ui/KeyBindings.h"
#include "../ui/LibrarySort.h"
#include "../ui/Notify.h"
#include "../ui/ContinueRow.h"
#include "../ui/TabRow.h"
#include "../ui/Sources.h"
#include "../ui/SettingsCard.h"
#include "../ui/SettingsPanel.h"
#include "../ui/Shortcuts.h"
#include "../ui/SidebarStyleCard.h"
#include "../ui/Theme.h"
#include "../ui/TileView.h"
#include "../ui/Tray.h"
#include "RunnersPage.h"
#include "SourcePage.h"
#include "SourceSettingsCard.h"

// setViewportMargins is protected on QAbstractScrollArea; this republishes
// it so ApplyLayoutTokens() can pad the tiles without also insetting the
// scrollbar. Ctrl+wheel resizes tiles instead of scrolling.
class LibraryGrid : public mira_gui::TileView {
public:
  using TileView::TileView;
  using TileView::setViewportMargins;

  // One call per notch, positive to grow. Set once by LibraryWindow.
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

// The sidebar's filter picker, inside the filter+sort popover. Status keys
// match mirad's `status` values; "all", "running" and "never" are
// frontend-only groupings.
struct FilterEntry {
  const char* label;
  const char* key;
  mira_gui::icons::Glyph icon;
};

const FilterEntry kFilters[] = {
    {"All games", "all", mira_gui::icons::Glyph::Filter},
    {"Playing now", "running", mira_gui::icons::Glyph::Play},
    {"Ready", "ready", mira_gui::icons::Glyph::CheckCircle},
    {"Needs install", "needs_install", mira_gui::icons::Glyph::Download},
    {"Setting up", "setting_up", mira_gui::icons::Glyph::Clock},
    {"Broken", "broken", mira_gui::icons::Glyph::Warning},
    {"Missing", "missing", mira_gui::icons::Glyph::CircleX},
    {"Never played", "never", mira_gui::icons::Glyph::Moon},
    // Every entry above excludes a hidden-tagged game; this is the only one
    // that shows them, and only them.
    {"Hidden", "hidden", mira_gui::icons::Glyph::EyeSlash},
    // After Hidden so Ctrl+1…9 keep their filters.
    {"Needs attention", "attention", mira_gui::icons::Glyph::Warning},
    {"Apps", "apps", mira_gui::icons::Glyph::Wrench},
};

// The filters the library's tab row offers, with its own shorter labels.
const std::pair<const char*, const char*> kFilterTabs[] = {
    {"all", "All"},           {"ready", "Installed"},  {"running", "Playing now"},
    {"attention", "Needs attention"}, {"never", "Never played"},
};

// A crash within this many seconds of launch reads as "failed to start".
constexpr std::int64_t kFailedStartSeconds = 30;

// A pinned game's tag. "favorite" because Lutris imports its favorites under it.
constexpr const char* kPinnedTag = "favorite";

bool HasTag(const mira_gui::GameSummary& game, const std::string& tag) {
  return std::find(game.tags.begin(), game.tags.end(), tag) != game.tags.end();
}

// Icon + label (label also stashed in Qt::UserRole + 1, for the pill) + a
// live count (see UpdateFilterCounts). Transparent background: the list's
// own selection highlight marks the active row.
QWidget* MakeFilterRow(mira_gui::icons::Glyph glyph, const QString& label, QWidget* parent) {
  auto* row = new QWidget(parent);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(6, 3, 6, 3);
  layout->setSpacing(8);
  auto* icon = new QLabel(row);
  icon->setObjectName("icon");
  icon->setPixmap(mira_gui::icons::For(glyph, mira_gui::theme::Current().text_muted).pixmap(14, 14));
  layout->addWidget(icon);
  auto* text = new QLabel(label, row);
  layout->addWidget(text, /*stretch=*/1);
  auto* count = new QLabel(row);
  count->setObjectName("count");
  count->setProperty("role", "muted");
  layout->addWidget(count);
  return row;
}

constexpr int kResizeMargin = 5;

Qt::Edges EdgesAt(const QSize& size, const QPoint& pos) {
  Qt::Edges edges;
  if (pos.x() <= kResizeMargin) edges |= Qt::LeftEdge;
  if (pos.x() >= size.width() - kResizeMargin) edges |= Qt::RightEdge;
  if (pos.y() <= kResizeMargin) edges |= Qt::TopEdge;
  if (pos.y() >= size.height() - kResizeMargin) edges |= Qt::BottomEdge;
  return edges;
}

// The frameless window's own background: a thin margin around the real
// content, the only thing left to grab for an edge resize with no OS
// titlebar. QWindow::startSystemResize hands the drag to the compositor,
// which is what makes this work under Wayland.
class RootWidget : public QWidget {
public:
  explicit RootWidget(QMainWindow* window) : window_(window) { setMouseTracking(true); }

protected:
  void mousePressEvent(QMouseEvent* event) override {
    QWindow* handle = window_->windowHandle();
    if (event->button() == Qt::LeftButton && !window_->isMaximized() && handle != nullptr) {
      const Qt::Edges edges = ResizableEdgesAt(event->pos());
      if (edges != Qt::Edges()) {
        handle->startSystemResize(edges);
        event->accept();
        return;
      }
      // The strip above the top bar moves the window like the bar does.
      if (event->pos().y() <= kResizeMargin) {
        handle->startSystemMove();
        event->accept();
        return;
      }
    }
    QWidget::mousePressEvent(event);
  }

  void mouseDoubleClickEvent(QMouseEvent* event) override {
    if (event->pos().y() <= kResizeMargin && ResizableEdgesAt(event->pos()) == Qt::Edges()) {
      window_->isMaximized() ? window_->showNormal() : window_->showMaximized();
      return;
    }
    QWidget::mouseDoubleClickEvent(event);
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (window_->isMaximized()) {
      unsetCursor();
      return;
    }
    const Qt::Edges edges = ResizableEdgesAt(event->pos());
    if ((edges & Qt::LeftEdge) && (edges & Qt::TopEdge)) {
      setCursor(Qt::SizeFDiagCursor);
    } else if ((edges & Qt::RightEdge) && (edges & Qt::BottomEdge)) {
      setCursor(Qt::SizeFDiagCursor);
    } else if ((edges & Qt::RightEdge) && (edges & Qt::TopEdge)) {
      setCursor(Qt::SizeBDiagCursor);
    } else if ((edges & Qt::LeftEdge) && (edges & Qt::BottomEdge)) {
      setCursor(Qt::SizeBDiagCursor);
    } else if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
      setCursor(Qt::SizeHorCursor);
    } else if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
      setCursor(Qt::SizeVerCursor);
    } else {
      unsetCursor();
    }
  }

private:
  // Edges under `pos` that resize. The top edge only resizes at its
  // corners; the rest of it moves the window, since a drag up there (often
  // toward the top of the screen, e.g. out of a tiled corner) is meant to
  // move it. Wayland doesn't tell a window where it is, so this can't
  // depend on the screen edges.
  Qt::Edges ResizableEdgesAt(const QPoint& pos) const {
    constexpr int kCorner = 14;
    Qt::Edges edges = EdgesAt(size(), pos);
    if (!(edges & Qt::TopEdge)) return edges;
    if (pos.x() <= kCorner) return Qt::TopEdge | Qt::LeftEdge;
    if (pos.x() >= width() - kCorner) return Qt::TopEdge | Qt::RightEdge;
    return edges & ~Qt::Edges(Qt::TopEdge);
  }

  QMainWindow* window_;
};

// Sidebar's filter+sort pill. Plain QWidget, not QPushButton: needs two
// icon+label pairs and a chevron, not one icon+text. Plain callback (like
// LibraryGrid), not a signal, since it is too small to need one.
class FilterSortButton : public QWidget {
public:
  explicit FilterSortButton(QWidget* parent) : QWidget(parent) {
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
  }
  std::function<void()> on_clicked;

protected:
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && on_clicked) on_clicked();
  }
};

// The game-edit card's dimmed backdrop. A click that lands here (never on
// the card itself, which is a child widget and consumes its own clicks
// first) closes the card, same as clicking outside any other modal.
class ModalOverlay : public QWidget {
public:
  explicit ModalOverlay(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_StyledBackground, true);
  }
  std::function<void()> on_backdrop_clicked;
  // Set: painted here instead of by a stylesheet, around `clear` (in this
  // widget's coordinates), which stays undimmed so its live changes show.
  QColor scrim;
  std::function<QRect()> clear;

protected:
  void paintEvent(QPaintEvent* event) override {
    if (!scrim.isValid()) return QWidget::paintEvent(event);
    QPainter painter(this);
    QRegion region(rect());
    if (clear) region -= clear();
    painter.setClipRegion(region);
    painter.fillRect(rect(), scrim);
  }

  // A press on the card's own empty space propagates up to here too, so
  // only one that lands on no child at all counts as the backdrop.
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() != Qt::LeftButton || !on_backdrop_clicked) return;
    if (childAt(event->position().toPoint()) != nullptr) return;
    on_backdrop_clicked();
  }
};

// A click on anything that can't take focus itself (a page's background, a
// label) drops keyboard focus from a text box or button, the way a browser
// does, so Enter and Delete go back to the window's own shortcuts.
class FocusDropper : public QObject {
public:
  explicit FocusDropper(QWidget* window) : QObject(window), window_(window) {}

protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (event->type() != QEvent::MouseButtonPress) return false;
    auto* target = qobject_cast<QWidget*>(watched);
    if (target == nullptr || target->window() != window_ || target->focusPolicy() & Qt::ClickFocus) return false;
    // A viewport forwards to its view, which takes focus on its own.
    if (qobject_cast<QAbstractScrollArea*>(target->parentWidget()) != nullptr &&
        target->parentWidget()->focusPolicy() & Qt::ClickFocus) {
      return false;
    }
    if (QWidget* focused = QApplication::focusWidget(); focused != nullptr && focused->window() == window_) {
      focused->clearFocus();
    }
    return false;
  }

private:
  QWidget* window_;
};

QLabel* SidebarHeading(QWidget* parent, const QString& text) {
  auto* label = new QLabel(text, parent);
  label->setProperty("role", "muted");
  label->setStyleSheet("font-weight: 600; letter-spacing: 0.04em; margin-top: 14px; margin-bottom: 2px;");
  return label;
}

// A muted label on the row's right, e.g. a game count.
QLabel* AddTrailingLabel(QPushButton* row) {
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 10, 0);
  layout->addStretch(1);
  auto* label = new QLabel(row);
  label->setProperty("role", "muted");
  label->setAttribute(Qt::WA_TransparentForMouseEvents);
  layout->addWidget(label);
  return label;
}

}  // namespace

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
  if (prefs.tile_width) tile_width_ = std::clamp(*prefs.tile_width, kMinTileWidth, kMaxTileWidth);
  if (prefs.sort_by) sort_key_ = *prefs.sort_by;
  sort_descending_ = prefs.sort_descending.value_or(false);
  for (const std::string& id : prefs.source_order.value_or(std::vector<std::string>{})) {
    source_order_.push_back(QString::fromStdString(id));
  }
  for (const auto& [id, at] : prefs.source_imported_at.value_or(std::map<std::string, std::int64_t>{})) {
    source_imported_at_[QString::fromStdString(id)] = at;
  }
  for (const auto& [id, width] : prefs.source_tile_widths.value_or(std::map<std::string, int>{})) {
    source_tile_widths_[id] = std::clamp(width, kMinTileWidth, kMaxTileWidth);
  }

  // The one copy of the library every view reads; see GameLibraryModel.
  library_ = new mira_gui::GameLibraryModel(this);
  library_->status_text = [this](const std::string& id) { return InstallText(id); };
  connect(library_, &mira_gui::GameLibraryModel::Changed, this, &LibraryWindow::LibraryChanged);
  grid_games_ = new mira_gui::GameFilterProxy(library_, this);
  grid_games_->SetSort(sort_key_, sort_descending_);

  // Before the panel and the grid, because both ask it for covers.
  artwork_ = new mira_gui::ArtworkStore(this);
  connect(artwork_, &mira_gui::ArtworkStore::CoverChanged, this, &LibraryWindow::UpdateTileCover);
  // Icons and hero art are only drawn in the sidebar.
  connect(artwork_, &mira_gui::ArtworkStore::SlotArtChanged, this, [this](const QString& id) {
    if (pinned_signature_.contains(id) || recent_signature_.contains(id)) RefreshSidebarGames();
  });

  InstallErrorNavigator();

  // Before the top bar, which shows its count.
  downloads_ = new mira_gui::DownloadTracker(this);
  downloads_->game_name = [this](const std::string& id) {
    const mira_gui::GameSummary* game = FindGame(id);
    return game != nullptr ? QString::fromStdString(game->name) : QString();
  };
  downloads_->source_name = [](const QString& id) {
    for (const mira_gui::SourceInfo& source : mira_gui::AllSources()) {
      if (source.id == id) return source.name;
    }
    return id;
  };
  connect(downloads_, &mira_gui::DownloadTracker::Changed, this, &LibraryWindow::DownloadChanged);
  downloads_panel_ = new mira_gui::DownloadsPanel(downloads_, artwork_, this);
  connect(downloads_panel_, &mira_gui::DownloadsPanel::ShowGameRequested, this,
          [this](const QString& id) { ShowGame(id.toStdString()); });

  // The stylesheet re-polishes every widget by itself; what it cannot reach
  // is what we paint: the tiles, and the placeholder covers drawn in the
  // theme's own colors.
  connect(mira_gui::theme::Notifier::Instance(), &mira_gui::theme::Notifier::Changed, this, [this] {
    artwork_->InvalidateAllRenderings();
    ApplyLayoutTokens();
    grid_->viewport()->update();
    LibraryChanged();  // the sidebar's rows are drawn in theme colors too
    ApplyTopBarIcons();
    UpdateLibraryNavActive();  // the checked rows' icons are on_accent
  });

  splitter_ = new QSplitter(Qt::Horizontal, this);
  splitter_->addWidget(BuildSidebar());
  // A source page or Runners takes the grid's place here, leaving the sidebar up.
  main_stack_ = new QStackedWidget(this);
  grid_page_ = BuildGrid();
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

  auto* central = new RootWidget(this);
  // StackAll: the game-edit overlay is a chrome sibling, not a
  // content_stack_ page, so the grid/sidebar stay visible (dimmed)
  // underneath. current_widget only picks which one is raised.
  root_stack_ = new QStackedLayout(central);
  root_stack_->setStackingMode(QStackedLayout::StackAll);
  root_stack_->setContentsMargins(0, 0, 0, 0);

  auto* chrome = new QWidget(central);
  auto* layout = new QVBoxLayout(chrome);
  layout->setContentsMargins(kResizeMargin, kResizeMargin, kResizeMargin, kResizeMargin);
  layout->setSpacing(0);
  QWidget* top_bar = BuildTopBar();
  // Without an explicit cursor here, a resize cursor RootWidget set at its
  // edge margin would keep showing over the whole window after the drag ends.
  top_bar->setCursor(Qt::ArrowCursor);
  layout->addWidget(top_bar);
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
    const int row = FilterRow(QString::fromStdString(*prefs.library_filter));
    if (row >= 0) filters_->setCurrentRow(row);
  }
  ApplySettingsPrefs(prefs);

  mira_gui::EventHub* hub = mira_gui::EventHub::Instance();
  connect(hub, &mira_gui::EventHub::Received, this,
          [this](const std::string& type, const std::string& data, bool live) { HandleGameEvent(type, data, live); });
  connect(hub, &mira_gui::EventHub::ConnectionChanged, this, &LibraryWindow::ConnectionChanged);
  hub->Start();

  qApp->installEventFilter(new FocusDropper(this));

  RefreshSourceNavs();
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
    auto* action = new QAction(grid_);
    const QKeySequence primary =
        mira_gui::keybindings::Register(action, id, label, default_keys, extra_aliases);
    QList<QKeySequence> keys{primary};
    keys.append(extra_aliases);
    action->setShortcuts(keys);
    action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(action, &QAction::triggered, this, slot);
    grid_->addAction(action);
  };

  window_action("focus_search", "Focus the search box", QKeySequence(QKeySequence::Find), {}, [this] {
    library_tabs_->OpenSearch();  // a narrow window shows only its button
    search_->setFocus(Qt::ShortcutFocusReason);
    search_->selectAll();
  });

  // One key, three jobs, in the order a user expects to undo them: leave
  // settings first, then clear the search, then clear the selection.
  window_action("clear_or_deselect", "Clear the search, then the selection",
               QKeySequence(Qt::Key_Escape), {}, [this] {
    if (SidebarCardOpen()) {
      CloseSidebarCard();
      return;
    }
    if (SettingsOpen()) {
      RequestCloseSettings();
      return;
    }
    if (GameEditOpen()) {
      GameEditBack();
      return;
    }
    if (!search_->text().isEmpty()) {
      search_->clear();
      return;
    }
    grid_->clearSelection();
    grid_->setCurrentIndex(QModelIndex());
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
  // by kFilters so adding a ninth filter cannot walk past Ctrl+9. Not
  // registered with keybindings: nine near-identical rebindable rows for
  // "pick the Nth filter" isn't worth the Settings screen space, and the
  // filter list itself isn't fixed enough to make good default labels for.
  for (int row = 0; row < filters_->count() && row < 9; ++row) {
    auto* action = new QAction(this);
    action->setShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + row)));
    connect(action, &QAction::triggered, this, [this, row] { filters_->setCurrentRow(row); });
    addAction(action);
  }

  // A dedicated toggle for Hidden, on top of whatever Ctrl+9 already gives
  // it. Toggles back to All on a second press so it never strands the grid.
  window_action("toggle_hidden", "Toggle the Hidden filter", QKeySequence(Qt::CTRL | Qt::Key_H), {},
               [this] {
                 const int hidden_row = FilterRow("hidden");
                 if (hidden_row < 0) return;
                 filters_->setCurrentRow(CurrentFilterKey() == "hidden" ? FilterRow("all")
                                                                        : hidden_row);
               });

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
               const mira_gui::GameSummary* game = FindGame(SelectedId());
               if (game == nullptr) return;
               // Same rule as the context menu's Play entry: a game that isn't
               // ready has nothing to launch.
               if (!game->running && game->status != "ready") return;
               ToggleRunning(std::string(game->id));
             });

  grid_action("details_settings", "Game settings", QKeySequence(Qt::ALT | Qt::Key_Return),
             {QKeySequence(Qt::ALT | Qt::Key_Enter)}, [this] {
               const std::string id = SelectedId();
               if (!id.empty()) OpenGameDialog(id);
             });

  grid_action("delete_game", "Remove the selected games", QKeySequence(Qt::Key_Delete), {}, [this] {
    // Copied before the call: the dialog's event loop can change the library.
    const std::vector<std::pair<std::string, QString>> selected = SelectedGames();
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
  // Set only when changed in Settings; the sidebar may have changed them since.
  if (prefs.hidden_sources) {
    hidden_sources_.clear();
    for (const std::string& id : *prefs.hidden_sources) hidden_sources_.insert(QString::fromStdString(id));
  }
  if (prefs.source_order) {
    source_order_.clear();
    for (const std::string& id : *prefs.source_order) source_order_.push_back(QString::fromStdString(id));
  }
  if (prefs.hidden_sources || prefs.source_order) UpdateSourceNavs();
  recent_count_ = prefs.sidebar_recent_count.value_or(0);
  show_source_counts_ = prefs.sidebar_source_counts.value_or(true);
  source_icons_ = prefs.sidebar_source_icons.value_or(true);
  pinned_style_ = mira_gui::sidebar::ParseStyle(prefs.sidebar_pinned_style.value_or("covers"));
  recent_style_ = mira_gui::sidebar::ParseStyle(prefs.sidebar_recent_style.value_or("covers"));
  recent_when_ = prefs.sidebar_recent_when.value_or(true);
  library_tabs_->SetTabsVisible(prefs.library_filter_tabs.value_or(true));
  continue_row_enabled_ = prefs.library_continue_row.value_or(true);
  continue_count_ = prefs.library_continue_count.value_or(3);
  source_page_tabs_ = prefs.source_page_tabs.value_or(true);
  tile_size_synced_ = prefs.tile_size_synced.value_or(false);
  delegate_->SetShowStatus(prefs.tile_status.value_or(true));
  delegate_->SetShowSourceMark(prefs.tile_source_mark.value_or(true));
  delegate_->SetShowPinBadge(prefs.tile_pin_badge.value_or(true));
  drag_select_ = prefs.drag_select.value_or(true);
  grid_->SetDragSelectEnabled(drag_select_);
  if (source_page_ != nullptr) source_page_->SetDragSelectEnabled(drag_select_);
  UpdateLibraryNavActive();  // the slider follows tile_size_synced_
  ApplyFilter();
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
      mira_gui::MiradClient::SaveFrontendPrefsAsync(this, LayoutPrefs(), [](mira_gui::PatchConfigResult) {});
    });
  }
  save_prefs_timer_->start();
}

void LibraryWindow::FlushPrefs() {
  if (save_prefs_timer_ == nullptr || !save_prefs_timer_->isActive()) return;
  save_prefs_timer_->stop();
  // Blocking: an async save's thread might not reach the socket before the
  // process exits. Failure isn't reported: the cost is a layout, not data.
  mira_gui::MiradClient::SaveFrontendPrefsBlocking(LayoutPrefs());
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
  prefs.tile_width = tile_width_;
  prefs.source_tile_widths = source_tile_widths_;
  prefs.library_filter = CurrentFilterKey().toStdString();
  prefs.sort_by = sort_key_;
  prefs.sort_descending = sort_descending_;
  const QList<int> sizes = splitter_->sizes();
  if (sizes.size() == 2) prefs.sidebar_width = sizes[0];
  return prefs;
}

void LibraryWindow::resizeEvent(QResizeEvent* event) {
  QMainWindow::resizeEvent(event);
  SizeGameEditCard(game_edit_card_);
  ScheduleSavePrefs();
}

void LibraryWindow::ApplyLayoutTokens() {
  if (grid_ == nullptr) return;
  // Viewport margins, not the container's contents margins: those would
  // inset the whole QListWidget frame, pushing its scrollbar in by the same
  // amount. This pads only the tiles' own drawing area, leaving the
  // scrollbar docked at the panel's true right edge.
  const int margin = mira_gui::theme::Current().grid_margin;
  grid_->setViewportMargins(margin, margin, margin, margin);
}

void LibraryWindow::ApplyTopBarIcons() {
  using mira_gui::icons::Glyph;
  settings_button_->setIcon(mira_gui::icons::For(Glyph::Settings));
  refresh_button_->setIcon(mira_gui::icons::For(Glyph::Refresh));
  downloads_button_->setIcon(mira_gui::icons::For(Glyph::Download));
  shortcuts_button_->setIcon(mira_gui::icons::For(Glyph::Keyboard));
  about_button_->setIcon(mira_gui::icons::For(Glyph::Info));
  top_bar_divider_->setStyleSheet(
      QString("background: %1;").arg(mira_gui::theme::Current().border.name()));
  minimize_button_->setIcon(mira_gui::icons::For(Glyph::Minimize));
  maximize_button_->setIcon(
      mira_gui::icons::For(isMaximized() ? Glyph::Restore : Glyph::Maximize));
  close_button_->setIcon(mira_gui::icons::For(Glyph::Close));
  add_games_->setIcon(mira_gui::icons::For(Glyph::Plus, mira_gui::theme::Current().on_accent));
  runners_nav_->setIcon(mira_gui::icons::For(Glyph::Wrench));
  fetch_art_button_->setIcon(mira_gui::icons::For(Glyph::Image));
  manage_sources_button_->setIcon(mira_gui::icons::For(Glyph::Sliders, mira_gui::theme::Current().text_muted));
  for (QToolButton* button : {pinned_customize_, recent_customize_}) {
    button->setIcon(mira_gui::icons::For(Glyph::Sliders, mira_gui::theme::Current().text_muted));
  }

  // The filter+sort pill's own static icons -- its text and the popover's
  // rows restyle separately (UpdateFilterSortSummary, restyle_filter_rows).
  if (filter_icon_ != nullptr) {
    const QColor muted = mira_gui::theme::Current().text_muted;
    filter_icon_->setPixmap(mira_gui::icons::For(Glyph::Filter, muted).pixmap(14, 14));
    sort_icon_->setPixmap(mira_gui::icons::For(Glyph::SortArrows, muted).pixmap(13, 13));
    filter_sort_chevron_->setPixmap(mira_gui::icons::For(Glyph::ChevronDown, muted).pixmap(12, 12));
  }
  UpdateFilterSortSummary();
}

void LibraryWindow::ToggleMaximize() {
  if (isMaximized()) {
    showNormal();
  } else {
    showMaximized();
  }
}

void LibraryWindow::changeEvent(QEvent* event) {
  if (event->type() == QEvent::WindowStateChange && maximize_button_ != nullptr) {
    maximize_button_->setIcon(mira_gui::icons::For(
        isMaximized() ? mira_gui::icons::Glyph::Restore : mira_gui::icons::Glyph::Maximize));
    maximize_button_->setToolTip(isMaximized() ? "Restore" : "Maximize");
    ScheduleSavePrefs();
  }
  QMainWindow::changeEvent(event);
}

bool LibraryWindow::eventFilter(QObject* watched, QEvent* event) {
  constexpr const char* kSourceMime = "application/x-mira-source";
  // Clicks in the grid's own padding (outside its viewport) or around it deselect.
  if ((watched == grid_ || watched == grid_page_) && event->type() == QEvent::MouseButtonPress &&
      static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
    ClearGridSelection();
    return false;
  }
  if (auto* nav = qobject_cast<QPushButton*>(watched); nav != nullptr && source_navs_.contains(nav)) {
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
      source_drag_row_ = nav;
      source_drag_start_ = mouse->position().toPoint();
    } else if (event->type() == QEvent::MouseMove && source_drag_row_ == nav &&
               (mouse->buttons() & Qt::LeftButton) &&
               (mouse->position().toPoint() - source_drag_start_).manhattanLength() >=
                   QApplication::startDragDistance()) {
      const qsizetype index = source_navs_.indexOf(nav);
      auto* mime = new QMimeData();
      mime->setData(kSourceMime, mira_gui::AllSources()[index].id.toUtf8());
      auto* drag = new QDrag(nav);
      drag->setMimeData(mime);
      drag->setPixmap(nav->grab());
      drag->setHotSpot(source_drag_start_);
      source_drag_row_ = nullptr;
      nav->setDown(false);
      drag->exec(Qt::MoveAction);
      return true;
    }
  }
  if (watched == source_nav_container_) {
    const auto* drop = static_cast<QDropEvent*>(event);
    switch (event->type()) {
      case QEvent::DragEnter:
      case QEvent::DragMove: {
        if (!drop->mimeData()->hasFormat(kSourceMime)) return false;
        event->accept();
        const int row = SourceDropRow(drop->position().toPoint().y());
        QWidget* anchor = nullptr;
        for (QPushButton* nav : source_navs_) {
          if (nav->isVisible() && source_nav_layout_->indexOf(nav) == row) anchor = nav;
        }
        int y = 0;
        if (anchor != nullptr) {
          y = anchor->geometry().top() - 2;
        } else {
          for (QPushButton* nav : source_navs_) {
            if (nav->isVisible()) y = std::max(y, nav->geometry().bottom());
          }
        }
        source_drop_line_->setGeometry(0, y, source_nav_container_->width(), 2);
        source_drop_line_->show();
        source_drop_line_->raise();
        return true;
      }
      case QEvent::DragLeave:
        source_drop_line_->hide();
        return true;
      case QEvent::Drop: {
        source_drop_line_->hide();
        if (!drop->mimeData()->hasFormat(kSourceMime)) return false;
        const QString id = QString::fromUtf8(drop->mimeData()->data(kSourceMime));
        MoveSource(id, SourceDropRow(drop->position().toPoint().y()));
        event->accept();
        return true;
      }
      default:
        break;
    }
  }
  // The top bar's own background, plus labels on it (a label passes its
  // clicks up); a click on a control goes to it instead.
  if (watched == top_bar_) {
    if (event->type() == QEvent::MouseButtonPress) {
      auto* mouse = static_cast<QMouseEvent*>(event);
      if (mouse->button() == Qt::LeftButton && windowHandle() != nullptr) {
        windowHandle()->startSystemMove();
        return true;
      }
    } else if (event->type() == QEvent::MouseButtonDblClick) {
      ToggleMaximize();
      return true;
    }
  }
  // A recently played row shows its game's hover card, after a tile's dwell.
  if (watched->property("hover_game").isValid()) {
    if (event->type() == QEvent::Enter) {
      if (recent_hover_ == nullptr) {
        recent_hover_ = new QTimer(this);
        recent_hover_->setSingleShot(true);
        recent_hover_->setInterval(mira_gui::card::kDwellMs);
        connect(recent_hover_, &QTimer::timeout, this, [this] {
          if (recent_hover_row_ == nullptr) return;
          const mira_gui::GameSummary* game =
              FindGame(recent_hover_row_->property("hover_game").toString().toStdString());
          if (game == nullptr) return;
          const QString hint = game->running             ? "Right-click to stop it."
                               : game->status == "ready" ? "Click to play."
                                                         : QString();
          ShowHoverCardFor(*game, QRect(recent_hover_row_->mapToGlobal(QPoint(0, 0)), recent_hover_row_->size()),
                           hint);
        });
      }
      recent_hover_row_ = qobject_cast<QWidget*>(watched);
      recent_hover_->start();
    } else if (event->type() == QEvent::Leave || event->type() == QEvent::MouseButtonPress) {
      if (recent_hover_ != nullptr) recent_hover_->stop();
      ShowHoverCard(QModelIndex());
    }
  }
  return QMainWindow::eventFilter(watched, event);
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
  const bool game_dirty =
      game_edit_form_ != nullptr && GameEditOpen() && game_edit_form_->IsDirty();
  if (settings_dirty || game_dirty) {
    switch (mira_gui::notify::ConfirmUnsaved(
        this, settings_dirty ? "Settings changed but not saved."
                             : "This game's edits aren't saved.")) {
      case mira_gui::notify::UnsavedAction::Cancel:
        event->ignore();
        return;
      case mira_gui::notify::UnsavedAction::SaveAndExit:
        event->ignore();
        // Neither Save() finishes synchronously, so quit for real only once it
        // has, via the one-shot below, not this closeEvent call.
        if (settings_dirty) {
          close_settings_after_save_ = true;
          connect(settings_panel_, &mira_gui::SettingsPanel::SaveFinished, this,
                  [this](bool ok, QString) {
                    if (ok) QuitOrClose();
                  },
                  Qt::SingleShotConnection);
          settings_panel_->Save();
        } else {
          connect(game_edit_form_, &mira_gui::GameEditForm::SaveFinished, this,
                  [this](bool ok, QString) {
                    if (ok) QuitOrClose();
                  },
                  Qt::SingleShotConnection);
          game_edit_form_->Save();
        }
        return;
      case mira_gui::notify::UnsavedAction::DiscardAndExit:
        break;  // fall through to the ordinary close below
    }
  }

  FlushPrefs();
  mira_gui::MiradClient::ClearArtThumbsBlocking();
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

void LibraryWindow::ScanLibrary() {
  mira_gui::MiradClient::ScanLibraryAsync(this, [this](mira_gui::ScanResult result) {
    if (!result.ok) {
      mira_gui::notify::FailedRequest(this, "Could not scan the library.", result.error);
      return;
    }
    // New games show up in the grid on their own; only "nothing happened"
    // has no visible result of its own.
    if (result.added == 0 && result.missing == 0 && result.restored == 0) {
      mira_gui::notify::Notice(this, "Scan finished. No changes.");
    }
  });
}

// Every import below reports its games as events, so none relists.
void LibraryWindow::ImportSteamLibrary() {
  mira_gui::MiradClient::ScanSteamAsync(this, [this](mira_gui::SteamScanResult result) {
    if (!result.ok) {
      mira_gui::notify::FailedRequest(this, "Could not import from Steam.", result.error);
      return;
    }
    NoteImported("steam");
    if (result.added == 0) mira_gui::notify::Notice(this, "No new Steam games found.");
  });
}

void LibraryWindow::ImportLutrisLibrary() {
  mira_gui::MiradClient::ImportLutrisAsync(this, [this](mira_gui::LutrisImportResult result) {
    if (!result.ok) {
      mira_gui::notify::FailedRequest(this, "Could not import from Lutris.", result.error);
      return;
    }
    NoteImported("lutris");
    QStringList skipped;
    if (result.other_runner > 0) {
      skipped << QString("%1 use a runner Mira leaves to Lutris (Steam, DOSBox, …)").arg(result.other_runner);
    }
    if (result.incomplete > 0) skipped << QString("%1 have a setup Mira can't import").arg(result.incomplete);
    QString text = result.added == 0 ? QString("No new Lutris games found.")
                                     : QString("Added %1 Lutris game%2.").arg(result.added).arg(result.added == 1 ? "" : "s");
    if (!skipped.isEmpty()) text += " Skipped: " + skipped.join("; ") + ".";
    if (result.added == 0 || !skipped.isEmpty()) mira_gui::notify::Notice(this, text);
  });
}

void LibraryWindow::ImportDesktopEntries() {
  mira_gui::DesktopEntryImportDialog dialog(this);
  dialog.exec();
}

void LibraryWindow::AddGameManually() {
  mira_gui::AddManualGameDialog dialog(this);
  dialog.exec();
}

void LibraryWindow::SyncDesktopEntries() {
  mira_gui::MiradClient::SyncDesktopEntriesAsync(this, [this](mira_gui::DesktopEntrySyncResult result) {
    if (!result.ok) {
      mira_gui::notify::FailedRequest(this, "Could not regenerate desktop entries.", result.error);
      return;
    }
    mira_gui::notify::Notice(this, "Desktop entries regenerated.");
  });
}

void LibraryWindow::RemoveAllDesktopEntries() {
  // desktop_entries.enabled is the only lever that actually makes Sync()
  // remove every mira-<id>.desktop entry rather than immediately rewriting
  // them (see desktop::DesktopEntries::Sync), so there's no "wipe once, stay
  // enabled" concept, so this is honest about turning the setting off too.
  if (!mira_gui::notify::Confirm(
          this, "Remove All Desktop Entries",
          "This turns off desktop entries and deletes every one Mira generated. "
          "Re-enable them any time in Settings → Desktop Entries.",
          "Remove all", /*destructive=*/true)) {
    return;
  }
  const mira_gui::ConfigEdit edit{"desktop_entries.enabled", "a boolean", "false"};
  mira_gui::MiradClient::PatchConfigAsync(
      this, {edit}, [this](mira_gui::PatchConfigResult patch_result) {
        if (!patch_result.ok) {
          mira_gui::notify::FailedRequest(this, "Could not turn off desktop entries.", patch_result.error);
          return;
        }
        mira_gui::MiradClient::SyncDesktopEntriesAsync(
            this, [this](mira_gui::DesktopEntrySyncResult sync_result) {
              if (!sync_result.ok) {
                mira_gui::notify::FailedRequest(this, "Could not remove the desktop entries.",
                                                sync_result.error);
                return;
              }
              mira_gui::notify::Notice(this, "Desktop entries removed.");
            });
      });
}

QWidget* LibraryWindow::BuildTopBar() {
  top_bar_ = new QWidget(this);
  top_bar_->setObjectName("top_bar");
  // Catches a press/double-click on the bar's own empty background; see
  // eventFilter. A click on any child widget never reaches here.
  top_bar_->installEventFilter(this);

  auto* layout = new QHBoxLayout(top_bar_);
  layout->setContentsMargins(12, 4, 6, 4);
  layout->setSpacing(8);

  // Labels pass their clicks up, so the brand drags the window like the bar.
  auto* badge = new QLabel("M", top_bar_);
  badge->setObjectName("brand_badge");
  badge->setFixedSize(22, 22);
  badge->setAlignment(Qt::AlignCenter);
  layout->addWidget(badge);
  auto* title = new QLabel("Mira", top_bar_);
  title->setObjectName("brand_title");
  layout->addWidget(title);

  layout->addStretch(1);

  zoom_ = new QSlider(Qt::Horizontal, top_bar_);
  zoom_->setRange(kMinTileWidth, kMaxTileWidth);
  zoom_->setValue(tile_width_);
  zoom_->setMaximumWidth(120);
  zoom_->setToolTip("Tile size");
  connect(zoom_, &QSlider::valueChanged, this, &LibraryWindow::Zoom);
  layout->addWidget(zoom_);

  downloads_button_ = new QToolButton(top_bar_);
  downloads_button_->setAutoRaise(true);
  downloads_button_->setToolTip("Activity");
  // Its count's text is taller than the icon; the bar shouldn't grow for it.
  downloads_button_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
  connect(downloads_button_, &QToolButton::clicked, this,
          [this] { downloads_panel_->ShowBelow(downloads_button_); });
  layout->addWidget(downloads_button_);

  // Moved from the sidebar's old hamburger menu -- generic actions that fit
  // the top bar (window chrome) better than a library-focused sidebar.
  refresh_button_ = new QToolButton(top_bar_);
  refresh_button_->setAutoRaise(true);
  refresh_button_->setToolTip("Refresh library");
  connect(refresh_button_, &QToolButton::clicked, this, [this] { Reload(/*force_scan=*/true); });
  layout->addWidget(refresh_button_);

  shortcuts_button_ = new QToolButton(top_bar_);
  shortcuts_button_->setAutoRaise(true);
  shortcuts_button_->setToolTip("Keyboard shortcuts");
  connect(shortcuts_button_, &QToolButton::clicked, this, [this] { common_.reference->trigger(); });
  layout->addWidget(shortcuts_button_);

  about_button_ = new QToolButton(top_bar_);
  about_button_->setAutoRaise(true);
  about_button_->setToolTip("About Mira");
  connect(about_button_, &QToolButton::clicked, this, &LibraryWindow::OpenAbout);
  layout->addWidget(about_button_);

  top_bar_divider_ = new QWidget(top_bar_);
  top_bar_divider_->setFixedSize(1, 20);
  layout->addWidget(top_bar_divider_);

  minimize_button_ = new QToolButton(top_bar_);
  minimize_button_->setAutoRaise(true);
  minimize_button_->setToolTip("Minimize");
  connect(minimize_button_, &QToolButton::clicked, this, &QWidget::showMinimized);
  layout->addWidget(minimize_button_);

  maximize_button_ = new QToolButton(top_bar_);
  maximize_button_->setAutoRaise(true);
  maximize_button_->setToolTip("Maximize");
  connect(maximize_button_, &QToolButton::clicked, this, &LibraryWindow::ToggleMaximize);
  layout->addWidget(maximize_button_);

  close_button_ = new QToolButton(top_bar_);
  close_button_->setAutoRaise(true);
  close_button_->setObjectName("close_button");
  close_button_->setToolTip("Close");
  connect(close_button_, &QToolButton::clicked, this, &QWidget::close);
  layout->addWidget(close_button_);

  ApplyTopBarIcons();
  return top_bar_;
}

QWidget* LibraryWindow::BuildFilterSortPopover() {
  using mira_gui::icons::Glyph;

  // Qt::Popup: grabs the mouse and closes itself on an outside click or
  // Escape, so the pill's on_clicked only ever needs to open it.
  auto* popover = new QWidget(this, Qt::Popup);
  popover->setObjectName("filter_sort_popover");
  auto* layout = new QVBoxLayout(popover);
  layout->setContentsMargins(8, 8, 8, 8);
  layout->setSpacing(2);

  auto* filter_heading = new QLabel("FILTER", popover);
  filter_heading->setProperty("role", "muted");
  filter_heading->setStyleSheet("font-weight: 600; letter-spacing: 0.04em;");
  layout->addWidget(filter_heading);

  filters_ = new QListWidget(popover);
  filters_->setObjectName("filter_list");
  filters_->setFrameShape(QFrame::NoFrame);
  filters_->setSelectionMode(QAbstractItemView::SingleSelection);
  filters_->setFocusPolicy(Qt::NoFocus);
  filters_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  for (const FilterEntry& entry : kFilters) {
    auto* item = new QListWidgetItem(filters_);
    item->setData(Qt::UserRole, QString(entry.key));
    // Stashed alongside the key so the pill can show it without digging
    // back into the row widget's own child labels.
    item->setData(Qt::UserRole + 1, QString(entry.label));
    auto* row = MakeFilterRow(entry.icon, entry.label, filters_);
    item->setSizeHint(row->sizeHint());
    filters_->setItemWidget(item, row);
  }
  // setItemWidget bypasses QSS's ::item:selected -- restyled by hand
  // instead (icon included), on_accent when active, else muted.
  auto restyle_filter_rows = [this] {
    for (int row = 0; row < filters_->count(); ++row) {
      QWidget* row_widget = filters_->itemWidget(filters_->item(row));
      const bool current = row == filters_->currentRow();
      const QColor color = current ? mira_gui::theme::Current().on_accent
                                   : mira_gui::theme::Current().text_muted;
      for (QLabel* label : row_widget->findChildren<QLabel*>()) {
        if (label->objectName() == "icon") {
          label->setPixmap(mira_gui::icons::For(kFilters[row].icon, color).pixmap(14, 14));
        } else {
          // Count included: on_accent for contrast against the accent
          // background, not left at its ordinary muted gray.
          label->setStyleSheet(current ? QString("color: %1;").arg(color.name()) : QString());
        }
      }
    }
  };
  filters_->setCurrentRow(0);
  restyle_filter_rows();
  connect(filters_, &QListWidget::currentRowChanged, this, [this, restyle_filter_rows] {
    restyle_filter_rows();
    if (library_tabs_ != nullptr) library_tabs_->SetCurrent(CurrentFilterKey());
    UpdateFilterSortSummary();
    ApplyFilter();
    ScheduleSavePrefs();
  });
  // QListWidget's own sizeHint doesn't grow with its item count -- fit
  // exactly the rows it has, once, rather than an arbitrary scrollable box.
  int filters_height = 2 * filters_->frameWidth();
  for (int row = 0; row < filters_->count(); ++row) filters_height += filters_->sizeHintForRow(row);
  filters_->setFixedHeight(filters_height);
  layout->addWidget(filters_);

  auto* divider = new QWidget(popover);
  divider->setFixedHeight(1);
  divider->setStyleSheet(QString("background: %1;").arg(mira_gui::theme::Current().border.name()));
  layout->addWidget(divider);

  auto* sort_heading_row = new QHBoxLayout();
  auto* sort_heading = new QLabel("SORT", popover);
  sort_heading->setProperty("role", "muted");
  sort_heading->setStyleSheet("font-weight: 600; letter-spacing: 0.04em;");
  sort_heading_row->addWidget(sort_heading, /*stretch=*/1);

  sort_direction_ = new QToolButton(popover);
  sort_direction_->setAutoRaise(true);
  connect(sort_direction_, &QToolButton::clicked, this, [this] {
    sort_descending_ = !sort_descending_;
    UpdateFilterSortSummary();
    ApplySort();
    ScheduleSavePrefs();
  });
  sort_heading_row->addWidget(sort_direction_);
  layout->addLayout(sort_heading_row);

  // Full-width rows, same shape as the filter list above (and the sidebar's
  // own nav rows) -- a segmented row cramped "Last played"/"Playtime" down
  // to unreadable widths at the sidebar's ~230px.
  auto* sort_group = new QButtonGroup(popover);
  sort_buttons_.clear();
  for (const mira_gui::SortOption& option : mira_gui::SortOptions()) {
    auto* button = new QPushButton(option.label, popover);
    button->setFlat(true);
    button->setCheckable(true);
    button->setChecked(option.key == sort_key_);
    const QString key = QString(option.key);
    connect(button, &QPushButton::clicked, this, [this, key] {
      sort_key_ = key.toStdString();
      UpdateFilterSortSummary();
      ApplySort();
      ScheduleSavePrefs();
    });
    sort_group->addButton(button);
    sort_buttons_.append(button);
    layout->addWidget(button);
  }

  return popover;
}

void LibraryWindow::UpdateFilterSortSummary() {
  if (filter_summary_label_ == nullptr) return;  // popover not built yet

  const QListWidgetItem* current = filters_->currentItem();
  filter_summary_label_->setText(current != nullptr ? current->data(Qt::UserRole + 1).toString()
                                                     : QString("All games"));

  QString sort_label = "Name";
  for (const mira_gui::SortOption& option : mira_gui::SortOptions()) {
    if (option.key == sort_key_) {
      sort_label = option.label;
      break;
    }
  }
  sort_summary_label_->setText(
      QString("%1 %2").arg(sort_label, sort_descending_ ? QString::fromUtf8("\xe2\x86\x93")
                                                        : QString::fromUtf8("\xe2\x86\x91")));
  // The popover's own button shows the current direction too.
  sort_direction_->setArrowType(sort_descending_ ? Qt::DownArrow : Qt::UpArrow);
  sort_direction_->setToolTip(sort_descending_ ? "Descending. Click for ascending."
                                               : "Ascending. Click for descending.");
}

QWidget* LibraryWindow::BuildSidebar() {
  auto* sidebar = new QWidget(this);
  sidebar->setObjectName("left_sidebar");
  // Rows with their own menu handle it first; the rest fall through to here.
  sidebar->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(sidebar, &QWidget::customContextMenuRequested, this,
          [this, sidebar](const QPoint& pos) { ShowSidebarMenu(sidebar->mapToGlobal(pos)); });
  auto* layout = new QVBoxLayout(sidebar);
  layout->setContentsMargins(10, 10, 10, 10);
  layout->setSpacing(2);

  // Always visible (not just a "back" affordance): checked/highlighted
  // exactly when the grid is the current content; see UpdateLibraryNavActive.
  library_nav_ = new QPushButton("Library", sidebar);
  library_nav_->setObjectName("library_nav");
  library_nav_->setFlat(true);
  library_nav_->setCheckable(true);
  library_nav_->setChecked(true);
  connect(library_nav_, &QPushButton::clicked, this, &LibraryWindow::ShowLibrary);
  layout->addWidget(library_nav_);

  runners_nav_ = new QPushButton("Runners", sidebar);
  runners_nav_->setFlat(true);
  runners_nav_->setCheckable(true);
  connect(runners_nav_, &QPushButton::clicked, this, &LibraryWindow::OpenRunners);
  layout->addWidget(runners_nav_);

  settings_button_ = new QPushButton("Settings", sidebar);
  settings_button_->setObjectName("sidebar_settings");
  settings_button_->setFlat(true);
  connect(settings_button_, &QPushButton::clicked, this, [this] { OpenSettings(); });
  layout->addWidget(settings_button_);

  // The PINNED, SOURCES and RECENTLY PLAYED rows scroll, so they never set
  // the window's minimum height.
  auto* nav_scroll = new QScrollArea(sidebar);
  nav_scroll->setWidgetResizable(true);
  nav_scroll->setFrameShape(QFrame::NoFrame);
  nav_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  nav_scroll->viewport()->setAutoFillBackground(false);
  auto* nav_content = new QWidget();
  nav_content->setAutoFillBackground(false);
  auto* nav_layout = new QVBoxLayout(nav_content);
  nav_layout->setContentsMargins(0, 0, 0, 0);
  nav_layout->setSpacing(2);

  // PINNED and RECENTLY PLAYED headings carry a button for the customize card.
  const auto game_heading = [this, nav_content](const QString& text, QToolButton*& button) {
    auto* heading = new QWidget(nav_content);
    auto* heading_layout = new QHBoxLayout(heading);
    heading_layout->setContentsMargins(0, 0, 0, 0);
    heading_layout->addWidget(SidebarHeading(heading, text), /*stretch=*/1);
    button = new QToolButton(heading);
    button->setAutoRaise(true);
    button->setToolTip("Customize how these look");
    connect(button, &QToolButton::clicked, this, &LibraryWindow::OpenSidebarStyle);
    heading_layout->addWidget(button, 0, Qt::AlignBottom);
    heading->setVisible(false);
    return heading;
  };
  pinned_heading_ = game_heading("PINNED", pinned_customize_);
  nav_layout->addWidget(pinned_heading_);
  pinned_layout_ = new QVBoxLayout();
  pinned_layout_->setSpacing(2);
  nav_layout->addLayout(pinned_layout_);

  auto* sources_heading = new QWidget(nav_content);
  auto* sources_heading_layout = new QHBoxLayout(sources_heading);
  sources_heading_layout->setContentsMargins(0, 0, 0, 0);
  sources_heading_layout->addWidget(SidebarHeading(sources_heading, "SOURCES"), /*stretch=*/1);
  manage_sources_button_ = new QToolButton(sources_heading);
  manage_sources_button_->setAutoRaise(true);
  manage_sources_button_->setToolTip("Manage sources");
  connect(manage_sources_button_, &QToolButton::clicked, this, &LibraryWindow::OpenManageSources);
  sources_heading_layout->addWidget(manage_sources_button_, 0, Qt::AlignBottom);
  nav_layout->addWidget(sources_heading);

  source_nav_layout_ = new QVBoxLayout();
  source_nav_layout_->setSpacing(4);
  // Rows drag to reorder; see eventFilter.
  source_nav_container_ = nav_content;
  nav_content->setAcceptDrops(true);
  nav_content->installEventFilter(this);
  source_drop_line_ = new QWidget(nav_content);
  source_drop_line_->setFixedHeight(2);
  source_drop_line_->setStyleSheet(QString("background: %1;").arg(mira_gui::theme::Current().accent.name()));
  source_drop_line_->hide();
  for (const mira_gui::SourceInfo& source : mira_gui::AllSources()) {
    auto* nav = new QPushButton(source.name, nav_content);
    nav->setFlat(true);
    nav->setCheckable(true);
    nav->setVisible(false);  // until UpdateSourceNavs knows it's set up
    nav->setObjectName("source_nav");
    nav->setIconSize(QSize(22, 22));
    source_counts_.append(AddTrailingLabel(nav));
    connect(nav, &QPushButton::clicked, this, [this, source] { OpenSource(source); });
    nav->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(nav, &QWidget::customContextMenuRequested, this,
            [this, nav, source](const QPoint& pos) { ShowSourceMenu(source, nav->mapToGlobal(pos)); });
    nav->installEventFilter(this);
    source_nav_layout_->addWidget(nav);
    source_navs_.append(nav);
  }
  nav_layout->addLayout(source_nav_layout_);

  recent_heading_ = game_heading("RECENTLY PLAYED", recent_customize_);
  nav_layout->addWidget(recent_heading_);
  recent_layout_ = new QVBoxLayout();
  recent_layout_->setSpacing(2);
  nav_layout->addLayout(recent_layout_);

  nav_layout->addStretch(1);
  nav_scroll->setWidget(nav_content);
  layout->addWidget(nav_scroll, /*stretch=*/1);

  auto* actions = new QHBoxLayout();
  actions->setSpacing(6);
  add_games_ = new QToolButton(sidebar);
  add_games_->setObjectName("add_games");
  add_games_->setText("Add games");
  add_games_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  add_games_->setPopupMode(QToolButton::InstantPopup);
  // QToolButton stays content-sized otherwise.
  add_games_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto* add_games_menu = new QMenu(add_games_);
  add_games_menu->addAction("Scan library folders", this, &LibraryWindow::ScanLibrary);
  add_games_menu->addAction("Import Steam library", this, &LibraryWindow::ImportSteamLibrary);
  add_games_menu
      ->addAction("Import Lutris games", this, &LibraryWindow::ImportLutrisLibrary)
      ->setToolTip(
          "Add the Wine games from Lutris's database. Nothing is moved or renamed, so the games "
          "stay playable in Lutris too.");
  add_games_menu
      ->addAction("Import desktop entries…", this, &LibraryWindow::ImportDesktopEntries)
      ->setToolTip(
          "Pick installed apps from your application menu to add as games. This includes "
          "Flatpak apps.");
  add_games_menu->addSeparator();
  add_games_menu->addAction("Add game manually…", this, &LibraryWindow::AddGameManually);
  add_games_->setMenu(add_games_menu);
  actions->addWidget(add_games_, /*stretch=*/1);

  fetch_art_button_ = new QToolButton(sidebar);
  fetch_art_button_->setObjectName("fetch_art");
  // mirad only fetches on its own for a newly found game, so one that failed
  // once stays bare until asked again.
  fetch_art_button_->setToolTip("Fetch missing cover art for every game without one");
  connect(fetch_art_button_, &QToolButton::clicked, this, &LibraryWindow::FetchMissingArtwork);
  actions->addWidget(fetch_art_button_);
  layout->addSpacing(6);
  layout->addLayout(actions);

  // Replaces the old bottom bar entirely.
  footer_ = new QLabel(sidebar);
  footer_->setProperty("role", "muted");
  footer_->setContentsMargins(6, 8, 6, 2);
  footer_->setWordWrap(true);
  layout->addWidget(footer_);

  return sidebar;
}

// The same tab row a source page has, holding the grid's filter, sort and
// search, then the Continue playing cards.
QWidget* LibraryWindow::BuildLibraryHeader() {
  auto* top = new QWidget(this);
  auto* layout = new QVBoxLayout(top);
  layout->setContentsMargins(11, 14, 11, 0);
  layout->setSpacing(14);

  library_tabs_ = new mira_gui::TabRow(top);
  for (const auto& [key, label] : kFilterTabs) library_tabs_->AddTab(key, label);
  library_tabs_->SetAlert("attention", true);
  library_tabs_->SetCurrent("all");
  connect(library_tabs_, &mira_gui::TabRow::CurrentChanged, this,
          [this](const QString& key) { filters_->setCurrentRow(FilterRow(key)); });

  // Pill summarizing filter+sort, opening a self-dismissing Qt::Popup with
  // the actual rows/buttons.
  filter_sort_popover_ = BuildFilterSortPopover();
  auto* pill = new FilterSortButton(top);
  pill->setObjectName("filter_sort_button");
  filter_sort_button_ = pill;
  auto* pill_layout = new QHBoxLayout(pill);
  pill->setFixedHeight(mira_gui::TabRow::kControlHeight);
  pill_layout->setContentsMargins(8, 0, 8, 0);
  pill_layout->setSpacing(6);
  filter_icon_ = new QLabel(pill);
  pill_layout->addWidget(filter_icon_);
  filter_summary_label_ = new QLabel(pill);
  pill_layout->addWidget(filter_summary_label_, /*stretch=*/1);
  auto* pill_divider = new QWidget(pill);
  pill_divider->setFixedSize(1, 14);
  pill_divider->setStyleSheet(QString("background: %1;").arg(mira_gui::theme::Current().border.name()));
  pill_layout->addWidget(pill_divider);
  sort_icon_ = new QLabel(pill);
  pill_layout->addWidget(sort_icon_);
  sort_summary_label_ = new QLabel(pill);
  sort_summary_label_->setProperty("role", "subtle");
  pill_layout->addWidget(sort_summary_label_);
  filter_sort_chevron_ = new QLabel(pill);
  pill_layout->addWidget(filter_sort_chevron_);
  pill->on_clicked = [this] {
    filter_sort_popover_->setFixedWidth(qMax(240, filter_sort_button_->width()));
    const QPoint below_left = filter_sort_button_->mapToGlobal(QPoint(0, filter_sort_button_->height() + 4));
    filter_sort_popover_->move(below_left);
    filter_sort_popover_->show();
  };
  library_tabs_->SetTrailing(pill);
  search_ = new QLineEdit(top);
  search_->setObjectName("library_search");
  search_->setPlaceholderText("Search…");
  search_->setClearButtonEnabled(true);
  connect(search_, &QLineEdit::textChanged, this, [this] { ApplyFilter(); });
  library_tabs_->SetSearch(search_);
  layout->addWidget(library_tabs_);

  continue_row_ = new mira_gui::ContinueRow(artwork_, top);
  continue_row_->setVisible(false);
  connect(continue_row_, &mira_gui::ContinueRow::PlayToggled, this,
          [this](const QString& id) { RowClicked(id.toStdString()); });
  connect(continue_row_, &mira_gui::ContinueRow::MenuRequested, this,
          [this](const QString& id, const QPoint& pos) { ShowGameMenu(id.toStdString(), pos); });
  layout->addWidget(continue_row_);
  return top;
}

QWidget* LibraryWindow::BuildGrid() {
  auto* container = new QWidget(this);
  grid_layout_ = new QVBoxLayout(container);
  QVBoxLayout* layout = grid_layout_;
  // Plus the grid's own padding and tile inset, lines up with the header.
  layout->setContentsMargins(11, 0, 11, 0);
  layout->setSpacing(6);
  layout->addWidget(BuildLibraryHeader());

  grid_ = new LibraryGrid(container);
  grid_->setObjectName("library_grid");
  delegate_ = new mira_gui::GameTileDelegate(grid_, TileSize(), artwork_);
  grid_->setItemDelegate(delegate_);
  grid_->setModel(grid_games_);
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
  connect(grid_, &QListView::customContextMenuRequested, this, &LibraryWindow::ShowContextMenu);
  connect(grid_, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex& index) {
    const mira_gui::GameSummary* game = grid_games_->GameAt(index);
    if (game == nullptr) return;
    const std::string id = game->id;
    // A game that still needs installing installs instead.
    if (game->status == "needs_install" && InstallText(id).isEmpty()) {
      OfferInstall(id);
      return;
    }
    // Same rule as the context menu's Play entry and the Enter shortcut: a
    // game that isn't ready has nothing to launch, and /launch would just 409.
    if (!game->running && game->status != "ready") return;
    ToggleRunning(id);
  });
  grid_->on_hover = [this](const QModelIndex& index) { ShowHoverCard(index); };
  grid_->on_ctrl_wheel = [this](int steps) {
    zoom_->setValue(zoom_->value() + steps * zoom_->pageStep());
  };
  layout->addWidget(grid_, /*stretch=*/1);
  ApplyLayoutTokens();  // needs grid_ to already exist
  // The padding around the tiles is background too; see eventFilter.
  grid_->installEventFilter(this);
  container->installEventFilter(this);

  empty_hint_ = new QLabel(container);
  empty_hint_->setAlignment(Qt::AlignCenter);
  empty_hint_->setProperty("role", "muted");
  empty_hint_->setVisible(false);
  layout->addWidget(empty_hint_);

  return container;
}

QSize LibraryWindow::TileSize() const {
  // 2:3 portrait cover ratio, plus room for the title band over the bottom.
  return QSize(tile_width_, tile_width_ * 3 / 2);
}

void LibraryWindow::Zoom(int width) {
  ScheduleSavePrefs();
  if (!SourcePageShown() || tile_size_synced_) SetTileWidth(width);
  if (!SourcePageShown()) return;
  if (!tile_size_synced_) source_tile_widths_[source_page_->property("source_id").toString().toStdString()] = width;
  source_page_->SetTileWidth(width);
}

int LibraryWindow::SourceTileWidth(const QString& id) const {
  if (tile_size_synced_) return tile_width_;
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
  zoom_->setValue(source ? SourceTileWidth(source_page_->property("source_id").toString()) : tile_width_);
  zoom_->setEnabled(!GameEditOpen() && (source || GridShown()));
}

void LibraryWindow::SetTileWidth(int width) {
  if (width == tile_width_) return;
  tile_width_ = width;
  // Every size the slider passes through would otherwise stay cached until quit.
  artwork_->InvalidateAllRenderings();
  delegate_->SetTileSize(TileSize());
  grid_->setGridSize(TileSize());
}

void LibraryWindow::UpdateTileCover(const QString& id) {
  if (source_page_ != nullptr) source_page_->UpdateCover(id);
  continue_row_->RefreshCover(id.toStdString());
  // The edit card draws the same game at another size and can't notice the
  // store changing, whether or not the game has a tile. A no-op for another game.
  if (game_edit_backdrop_ != nullptr) game_edit_backdrop_->RefreshCover(id.toStdString());
  if (game_edit_cover_ != nullptr) game_edit_cover_->RefreshCover(id.toStdString());
  // Sidebar rows draw the cover, or take their color from it.
  if (pinned_signature_.contains(id) || recent_signature_.contains(id)) RefreshSidebarGames();
  // Every view of that game repaints its row.
  library_->Touch(id.toStdString());
}

void LibraryWindow::InstallErrorNavigator() {
  const auto find_source = [](const std::string& id) -> const mira_gui::SourceInfo* {
    const auto& sources = mira_gui::AllSources();
    const auto it = std::ranges::find(sources, QString::fromStdString(id), &mira_gui::SourceInfo::id);
    return it != sources.end() ? &*it : nullptr;
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
    mira_gui::MiradClient::InstallGameAsync(self, id, /*interactive=*/true, std::string(),
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
  // One job for the whole library; mirad decides what's missing, and Activity shows it going.
  // Asked for, so a missing SteamGridDB key is worth saying (ShowSteamGridDbNotice).
  artwork_fetch_requested_ = true;
  mira_gui::MiradClient::RefreshMissingArtworkAsync(this, [this](mira_gui::MetadataBatchResult result) {
    artwork_fetch_requested_ = false;
    if (!result.ok) {
      mira_gui::notify::FailedRequest(this, "Could not fetch missing cover art.", result.error);
    } else if (result.refreshed + result.failed == 0) {
      mira_gui::notify::Notice(this, "Every game already has cover art.");
    } else {
      mira_gui::notify::Notice(this, mira_gui::BatchRefreshSummary(result));
    }
  });
}

void LibraryWindow::RefreshMetadata(const std::string& id, bool announce) {
  mira_gui::MiradClient::RefreshMetadataAsync(
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
    mira_gui::MiradClient::ScanLibraryAsync(this, [](mira_gui::ScanResult) {});
  }
}

void LibraryWindow::RefreshGames() {
  // Hidden games included, so Ctrl+H is a client-side filter switch, not a round trip.
  const int request = ++games_request_;
  mira_gui::MiradClient::ListAllGamesAsync(this, [this, request](mira_gui::GamesResult result) {
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
  // mirad restarted or came back: what changed meanwhile may be past its replay.
  if (std::exchange(stream_dropped_, false)) {
    RefreshGames();
    RefreshSourceNavs();
    downloads_->RecheckJobs();
  }
}

QString LibraryWindow::CurrentFilterKey() const {
  QListWidgetItem* item = filters_->currentItem();
  return item != nullptr ? item->data(Qt::UserRole).toString() : QString("all");
}

int LibraryWindow::FilterRow(const QString& key) const {
  for (int row = 0; row < filters_->count(); ++row) {
    if (filters_->item(row)->data(Qt::UserRole).toString() == key) return row;
  }
  return -1;
}

void LibraryWindow::UpdateFilterCounts() {
  for (int row = 0; row < filters_->count(); ++row) {
    QListWidgetItem* item = filters_->item(row);
    const QString key = item->data(Qt::UserRole).toString();
    const auto count = std::ranges::count_if(
        library_->Games(), [&key](const mira_gui::GameSummary& game) { return mira_gui::GameFilterProxy::MatchesKey(game, key); });
    auto* count_label = qobject_cast<QLabel*>(filters_->itemWidget(item)->findChild<QLabel*>("count"));
    if (count_label != nullptr) count_label->setText(QString::number(count));
    library_tabs_->SetCount(key, count);
  }
}

void LibraryWindow::ApplyFilter() {
  // Rows the proxy drops leave the selection; nothing else moves.
  const QString key = CurrentFilterKey();
  const QString search = search_->text();
  grid_games_->SetFilterKey(key);
  grid_games_->SetSearch(search);
  grid_->scrollToTop();  // a new filter or search starts at the top
  UpdateEmptyState();
  RefreshSidebarGames();  // pinned rows follow the Hidden filter
  RefreshContinue();  // only shown under All with no search
}

void LibraryWindow::ApplySort() {
  grid_games_->SetSort(sort_key_, sort_descending_);
  grid_->scrollToTop();
}

void LibraryWindow::LibraryChanged() {
  // Its game was removed elsewhere (a source, the CLI): saving would only fail.
  if (game_edit_form_ != nullptr && library_->Find(game_edit_form_->id()) == nullptr) CloseGameEdit();
  UpdateFilterCounts();
  UpdateEmptyState();
  if (runners_page_ != nullptr) runners_page_->SetGames(library_->Games());
  UpdateSourceNavs();
  RefreshSidebarGames();
  RefreshContinue();
}

void LibraryWindow::UpdateEmptyState() {
  const int shown = grid_games_->rowCount();
  empty_hint_->setVisible(shown == 0);
  if (shown == 0) {
    empty_hint_->setText(library_->Games().empty() ? "No games in the library yet." : "No games match this filter.");
  }
  UpdateFooter();
}

void LibraryWindow::UpdateFooter() {
  // The connection only earns a line when it's gone.
  QString text = QString("%1 of %2 games shown").arg(grid_games_->rowCount()).arg(library_->Games().size());
  if (!mirad_reachable_) {
    text += QString("<br><span style='color:%1'>●</span> mirad isn't running")
                .arg(mira_gui::theme::Current().error.name());
  }
  footer_->setText(text);
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

void LibraryWindow::ClearGridSelection() {
  grid_->clearSelection();
  grid_->setCurrentIndex(QModelIndex());
}

std::vector<std::pair<std::string, QString>> LibraryWindow::SelectedGames() const {
  // In grid order, not the order they were picked in.
  QModelIndexList rows = grid_->selectionModel()->selectedIndexes();
  std::ranges::sort(rows, [](const QModelIndex& a, const QModelIndex& b) { return a.row() < b.row(); });
  std::vector<std::pair<std::string, QString>> games;
  for (const QModelIndex& index : rows) {
    games.emplace_back(index.data(mira_gui::GameTileDelegate::IdRole).toString().toStdString(),
                       index.data(mira_gui::GameTileDelegate::NameRole).toString());
  }
  return games;
}

std::string LibraryWindow::SelectedId() const {
  const QModelIndexList rows = grid_->selectionModel()->selectedIndexes();
  return rows.size() == 1 ? rows.front().data(mira_gui::GameTileDelegate::IdRole).toString().toStdString()
                          : std::string();
}

void LibraryWindow::ShowHoverCard(const QModelIndex& index) {
  if (!index.isValid()) {
    if (hover_card_ != nullptr) hover_card_->hide();
    return;
  }
  // Nothing to preview once the grid isn't on screen, and a preview for one
  // game reads as wrong noise over an active multi-selection.
  if (!GridShown() || grid_->selectionModel()->selectedIndexes().size() > 1) return;
  const mira_gui::GameSummary* game = grid_games_->GameAt(index);
  if (game == nullptr) return;
  const QRect tile = grid_->visualRect(index);
  ShowHoverCardFor(*game, QRect(grid_->viewport()->mapToGlobal(tile.topLeft()), tile.size()));
}

void LibraryWindow::ShowHoverCardFor(const mira_gui::GameSummary& game, const QRect& anchor,
                                     const QString& hint) {
  if (hover_card_ == nullptr) hover_card_ = new mira_gui::HoverCard(this);
  hover_card_->ShowGame(game, game.running, hint);
  hover_card_->PopUpBeside(anchor);
}

void LibraryWindow::ShowContextMenu(const QPoint& pos) {
  const QModelIndex index = grid_->indexAt(pos);
  if (!index.isValid()) return;
  // Right-clicking outside the current selection replaces it, same as most
  // file managers; right-clicking inside a multi-selection keeps it so the
  // batch menu below applies to everything that was selected.
  if (!grid_->selectionModel()->isSelected(index)) {
    grid_->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect);
  }
  // NoUpdate: the default ClearAndSelect would drop a drag or Ctrl+A selection
  // whenever the right-clicked tile isn't already the current one.
  grid_->selectionModel()->setCurrentIndex(index, QItemSelectionModel::NoUpdate);

  if (const QModelIndexList selected = grid_->selectionModel()->selectedIndexes(); selected.size() > 1) {
    std::vector<std::string> ids;
    for (const QModelIndex& index : selected) {
      ids.push_back(index.data(mira_gui::GameTileDelegate::IdRole).toString().toStdString());
    }
    ShowBatchMenu(ids, grid_->viewport()->mapToGlobal(pos));
    return;
  }

  ShowGameMenu(index.data(mira_gui::GameTileDelegate::IdRole).toString().toStdString(),
               grid_->viewport()->mapToGlobal(pos));
}

void LibraryWindow::ShowGameMenu(const std::string& id, const QPoint& global_pos,
                                 const std::function<void(QMenu&)>& extra) {
  // Copied, not kept as a pointer: an event landing while the menu is open can move the library's rows.
  const mira_gui::GameSummary* found = FindGame(id);
  if (found == nullptr) return;
  const mira_gui::GameSummary game = *found;
  const QString name = QString::fromStdString(game.name);
  const std::string& status = game.status;
  const bool running = game.running;

  QMenu menu(this);
  QAction* play = menu.addAction(running ? "Stop" : "Play");
  play->setEnabled(running || status == "ready");
  if (extra) extra(menu);
  QAction* details = menu.addAction("Game settings…");
  QAction* folder = menu.addAction("Open install folder");
  QAction* more_details = menu.addAction("More details…");
  const bool pinned = HasTag(game, kPinnedTag);
  QAction* toggle_pinned = menu.addAction(pinned ? "Unpin" : "Pin to sidebar");
  menu.addSeparator();
  // Both halves of the needs_install escape hatch: run the installer inside
  // this game's prefix, then say it worked. "Run in prefix" is offered for
  // every game; only needs_install can be "marked installed".
  const bool installing = !InstallText(id).isEmpty();
  QAction* install = menu.addAction(installing ? "Installing…" : "Install…");
  install->setEnabled((status == "needs_install" || status == "broken") && !installing);
  install->setToolTip("Run this game's installer, or pick a different one");
  QAction* run_in_prefix = menu.addAction("Run in prefix…");
  QAction* finish_install = menu.addAction("Mark as installed");
  finish_install->setEnabled(status == "needs_install");
  finish_install->setToolTip(status == "needs_install"
                                 ? "Mark this game as ready once its executable points at the "
                                   "installed program"
                                 : "Only available for a game that still needs installing");
  QAction* refresh_metadata = menu.addAction("Refresh metadata && cover art");
  QAction* view_log = menu.addAction("View log…");
  QAction* winetricks = menu.addAction("Run winetricks…");
  QAction* relocate = menu.addAction("Move to Mira's folders…");
  relocate->setToolTip("Move this game's files and prefix into the library and prefix folders");
  const bool native = game.platform == "native";
  winetricks->setEnabled(!native);
  winetricks->setToolTip(native ? "Native games have no Wine or Proton prefix." : QString());
  // The resolved setting isn't on GameSummary. The menu opens at once and the
  // entry fills in when mirad answers, a moment later.
  QAction* desktop_entry = menu.addAction("Desktop entry");
  desktop_entry->setEnabled(false);
  auto desktop_entry_enabled = std::make_shared<bool>(true);
  mira_gui::MiradClient::GetGameConfigAsync(
      &menu, id, [desktop_entry, desktop_entry_enabled](mira_gui::GameConfigResult result) {
        if (!result.ok) return;
        const auto entry = std::ranges::find(result.entries, std::string("desktop_entries.enabled"),
                                             &mira_gui::GameConfigEntry::key);
        if (entry == result.entries.end()) return;
        *desktop_entry_enabled = entry->value_display == "true";
        desktop_entry->setText(*desktop_entry_enabled ? "Remove desktop entry" : "Add desktop entry");
        desktop_entry->setEnabled(true);
      });
  menu.addSeparator();
  const bool hidden = HasTag(game, "hidden");
  QAction* toggle_hidden = menu.addAction(hidden ? "Unhide" : "Hide");
  toggle_hidden->setToolTip(hidden
                                ? "Show this game in the library again"
                                : "Keep this game out of the library until you ask for it "
                                  "(Ctrl+H, or the Hidden filter)");
  const bool app = mira_gui::IsApp(game);
  QAction* toggle_app = menu.addAction(app ? "Mark as game" : "Mark as app");
  toggle_app->setToolTip("An app is a program rather than a game: no playtime, and kept out of Continue");
  menu.addSeparator();
  QAction* remove = menu.addAction("Remove from library…");

  QAction* chosen = menu.exec(global_pos);
  if (chosen == play) {
    ToggleRunning(id);
  } else if (chosen == details) {
    OpenGameDialog(id);
  } else if (chosen == folder) {
    mira_gui::actions::OpenInstallFolder(this, game.install_path);
  } else if (chosen == more_details) {
    OpenGameDetailPage(id);
  } else if (chosen == install) {
    OfferInstall(id);
  } else if (chosen == relocate) {
    mira_gui::actions::Relocate(this, {{id, name}}, nullptr);
  } else if (chosen == run_in_prefix) {
    mira_gui::actions::RunInPrefix(this, id, game.install_path, name);
  } else if (chosen == finish_install) {
    mira_gui::actions::FinishInstall(this, id, nullptr);
  } else if (chosen == refresh_metadata) {
    RefreshMetadata(id);
  } else if (chosen == view_log) {
    mira_gui::actions::ViewLog(this, id, name);
  } else if (chosen == winetricks) {
    mira_gui::actions::RunWinetricks(this, id, name);
  } else if (chosen == desktop_entry) {
    mira_gui::actions::ToggleDesktopEntry(this, id, *desktop_entry_enabled);
  } else if (chosen == toggle_pinned) {
    ToggleTag(id, kPinnedTag);
  } else if (chosen == toggle_hidden) {
    ToggleTag(id, "hidden");
  } else if (chosen == toggle_app) {
    ToggleTag(id, "app");
  } else if (chosen == remove) {
    mira_gui::actions::Delete(this, id, name, [this, id] { RemoveGame(id); });
  }
}

void LibraryWindow::ShowBatchMenu(const std::vector<std::string>& ids, const QPoint& global_pos) {
  // Named before the menu opens: an event landing while it's up can replace games_.
  std::vector<std::pair<std::string, QString>> named;
  named.reserve(ids.size());
  for (const std::string& id : ids) {
    const mira_gui::GameSummary* game = FindGame(id);
    named.emplace_back(id, game != nullptr ? QString::fromStdString(game->name) : QString::fromStdString(id));
  }
  const int count = static_cast<int>(ids.size());

  int pinned = 0;
  int hidden = 0;
  int apps = 0;
  for (const std::string& id : ids) {
    const mira_gui::GameSummary* game = FindGame(id);
    if (game != nullptr && HasTag(*game, kPinnedTag)) ++pinned;
    if (game != nullptr && HasTag(*game, "hidden")) ++hidden;
    if (game != nullptr && mira_gui::IsApp(*game)) ++apps;
  }

  // Each offered for the games it would change, so a mixed selection gets both.
  QMenu menu(this);
  QAction* refresh_metadata = menu.addAction(QString("Refresh metadata && cover art (%1)").arg(count));
  QAction* pin = pinned < count ? menu.addAction(QString("Pin to sidebar (%1)").arg(count - pinned)) : nullptr;
  QAction* unpin = pinned > 0 ? menu.addAction(QString("Unpin (%1)").arg(pinned)) : nullptr;
  QAction* hide = hidden < count ? menu.addAction(QString("Hide (%1)").arg(count - hidden)) : nullptr;
  if (hide != nullptr) {
    hide->setToolTip("Keep these games out of the library until you ask for them (Ctrl+H, or the Hidden filter)");
  }
  QAction* unhide = hidden > 0 ? menu.addAction(QString("Unhide (%1)").arg(hidden)) : nullptr;
  QAction* mark_app = apps < count ? menu.addAction(QString("Mark as app (%1)").arg(count - apps)) : nullptr;
  QAction* mark_game = apps > 0 ? menu.addAction(QString("Mark as game (%1)").arg(apps)) : nullptr;
  auto* desktop_menu = menu.addMenu("Desktop entry");
  QAction* add_desktop_entry = desktop_menu->addAction("Add to application menu");
  QAction* remove_desktop_entry = desktop_menu->addAction("Remove from application menu");
  QAction* relocate = menu.addAction(QString("Move to Mira's folders… (%1)").arg(count));
  menu.addSeparator();
  QAction* remove = menu.addAction(QString("Remove from library… (%1)").arg(count));

  QAction* chosen = menu.exec(global_pos);
  if (chosen == nullptr) return;  // dismissed; also keeps it from matching an action left out above
  if (chosen == refresh_metadata) {
    // Activity shows it going; a notice sums it up at the end.
    mira_gui::MiradClient::RefreshMetadataManyAsync(this, ids, [this](mira_gui::MetadataBatchResult result) {
      if (!result.ok) {
        mira_gui::notify::FailedRequest(this, "Could not refresh metadata.", result.error);
      } else {
        mira_gui::notify::Notice(this, mira_gui::BatchRefreshSummary(result));
      }
    });
  } else if (chosen == pin || chosen == unpin) {
    BatchSetTag(ids, kPinnedTag, chosen == pin);
  } else if (chosen == hide || chosen == unhide) {
    BatchSetTag(ids, "hidden", chosen == hide);
  } else if (chosen == mark_app || chosen == mark_game) {
    BatchSetTag(ids, "app", chosen == mark_app);
  } else if (chosen == add_desktop_entry) {
    mira_gui::actions::BatchSetDesktopEntry(this, ids, /*enabled=*/true);
  } else if (chosen == remove_desktop_entry) {
    mira_gui::actions::BatchSetDesktopEntry(this, ids, /*enabled=*/false);
  } else if (chosen == relocate) {
    mira_gui::actions::Relocate(this, named, nullptr);
  } else if (chosen == remove) {
    mira_gui::actions::BatchDelete(this, named, nullptr);
  }
}

void LibraryWindow::BatchSetTag(const std::vector<std::string>& ids, const std::string& tag, bool present) {
  mira_gui::GamesPatch patch;
  for (const std::string& id : ids) {
    const mira_gui::GameSummary* game = FindGame(id);
    if (game != nullptr && HasTag(*game, tag) != present) patch.ids.push_back(id);
  }
  if (patch.ids.empty()) return;
  (present ? patch.add_tags : patch.remove_tags).push_back(tag);

  const bool one = patch.ids.size() == 1;
  mira_gui::MiradClient::PatchGamesAsync(this, patch, [this, tag, one](mira_gui::PatchGamesResult result) {
    if (!result.ok) {
      const QString games = one ? "this game's" : "these games'";
      mira_gui::notify::FailedRequest(this,
                                      tag == "hidden" ? QString("Could not change %1 visibility.").arg(games)
                                      : tag == "app"  ? QString("Could not change what %1 marked as.")
                                                            .arg(one ? "this is" : "these are")
                                                      : QString("Could not change whether %1 pinned.")
                                                            .arg(one ? "this game is" : "these games are"),
                                      result.error);
      return;
    }
    // Applied from the reply rather than waiting for games.updated, so the
    // change feels instant. No toast: the games visibly moving is the feedback.
    UpsertGames(result.games);
  });
}

void LibraryWindow::ToggleTag(const std::string& id, const std::string& tag) {
  const mira_gui::GameSummary* game = FindGame(id);
  if (game != nullptr) BatchSetTag({id}, tag, !HasTag(*game, tag));
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
              mira_gui::MiradClient::FinishInstallAsync(
                  this, id,
                  [this, id, is_app](mira_gui::FinishInstallResult result) {
                    if (!result.ok) {
                      mira_gui::notify::FailedRequest(this, "Could not switch to the installed program.",
                                                      result.error);
                      return;
                    }
                    if (is_app) BatchSetTag({id}, "app", true);
                  },
                  install_path, exe_path);
            });
    ShowSidebarCard(card);
  });
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
  // Fresh instance each time: GameEditForm loads its id at construction.
  if (game_edit_card_ != nullptr) CloseGameEdit();
  SetGridControlsEnabled(false);
  game_edit_card_ = BuildGameEditCard(id);
  game_edit_overlay_layout_->addWidget(game_edit_card_, 0, 0, Qt::AlignCenter);
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
  if (game_edit_card_ != nullptr) {
    game_edit_overlay_layout_->removeWidget(game_edit_card_);
    game_edit_card_->deleteLater();
    game_edit_card_ = nullptr;
    game_edit_form_ = nullptr;
    game_edit_backdrop_ = nullptr;
    game_edit_cover_ = nullptr;
    game_edit_stack_ = nullptr;
    game_edit_picker_ = nullptr;
    game_edit_title_ = nullptr;
    game_edit_art_button_ = nullptr;
    game_edit_play_ = nullptr;
    game_edit_bar_ = nullptr;
  }
  ShowNextCard();
}

bool LibraryWindow::ArtPickerOpen() const {
  return game_edit_picker_ != nullptr && game_edit_stack_->currentWidget() == game_edit_picker_;
}

void LibraryWindow::OpenArtPicker() {
  if (game_edit_form_ == nullptr || ArtPickerOpen()) return;
  if (game_edit_picker_ == nullptr) {
    game_edit_picker_ = new mira_gui::ArtPickerPanel(game_edit_form_->id(), game_edit_stack_);
    game_edit_stack_->addWidget(game_edit_picker_);
    connect(game_edit_picker_, &mira_gui::ArtPickerPanel::Previewed, this,
            [this](const QString& preview_slot, const QPixmap& preview) {
              game_edit_backdrop_->SetPreview(preview_slot, preview);
              if (preview_slot == "cover") game_edit_cover_->SetPreview(preview);
            });
    // The change bar is the picker's only while it's open; an apply can land after.
    connect(game_edit_picker_, &mira_gui::ArtPickerPanel::PickChanged, this, [this](bool has_change) {
      if (!ArtPickerOpen()) return;
      const bool hero = game_edit_picker_->slot() == "hero";
      game_edit_bar_->SetText(has_change ? (hero ? "New hero art picked" : "New cover picked") : QString(),
                              hero ? "Use this hero" : "Use this cover", "Cancel");
    });
    connect(game_edit_picker_, &mira_gui::ArtPickerPanel::PickActivated, this, [this] {
      if (!ArtPickerOpen()) return;
      game_edit_picker_->Apply();
      CloseArtPicker(/*applied=*/true);
    });
    connect(game_edit_picker_, &mira_gui::ArtPickerPanel::ApplyFailed, this,
            [this](const QString& failed_slot, const QString& error) {
              if (game_edit_backdrop_ != nullptr) game_edit_backdrop_->SetPreview(failed_slot, QPixmap());
              if (game_edit_cover_ != nullptr && failed_slot == "cover") game_edit_cover_->SetPreview(QPixmap());
              mira_gui::notify::Failed(this, "Could not change the art.", error);
            });
  }
  game_edit_art_button_->setChecked(true);
  game_edit_bar_->SetText(QString());
  game_edit_stack_->setCurrentWidget(game_edit_picker_);
  game_edit_picker_->Open("cover");
}

void LibraryWindow::CloseArtPicker(bool applied) {
  if (!ArtPickerOpen()) return;
  // An applied pick stays on screen until its art arrives in its place.
  if (!applied) {
    game_edit_backdrop_->SetPreview(QString::fromStdString(game_edit_picker_->slot()), QPixmap());
    game_edit_cover_->SetPreview(QPixmap());
  }
  game_edit_art_button_->setChecked(false);
  game_edit_stack_->setCurrentIndex(0);
  UpdateGameEditBar();
}

void LibraryWindow::UpdateGameEditBar() {
  game_edit_bar_->SetCount(game_edit_form_->ChangeCount());
  game_edit_form_->SetBottomRoom(game_edit_bar_->isHidden() ? 0 : game_edit_bar_->RoomNeeded());
}

void LibraryWindow::GameEditBack() {
  if (ArtPickerOpen()) {
    CloseArtPicker();
  } else if (game_edit_form_ != nullptr && game_edit_form_->AdvancedOpen()) {
    game_edit_form_->CloseAdvanced();
  } else {
    RequestCloseGameEdit();
  }
}

void LibraryWindow::UpdateGameEditPlay() {
  if (game_edit_play_ == nullptr || game_edit_form_ == nullptr) return;
  const mira_gui::GameSummary* game = FindGame(game_edit_form_->id());
  const bool running = game != nullptr && game->running;
  game_edit_play_->setText(running ? "Stop" : "Play");
  // Same rule as the context menu's Play entry.
  game_edit_play_->setEnabled(game != nullptr && (running || game->status == "ready"));
}

void LibraryWindow::RequestCloseGameEdit() {
  if (game_edit_form_ == nullptr || !game_edit_form_->IsDirty()) {
    CloseGameEdit();
    return;
  }
  switch (mira_gui::notify::ConfirmUnsaved(this, "This game's edits aren't saved.")) {
    case mira_gui::notify::UnsavedAction::Cancel:
      return;
    case mira_gui::notify::UnsavedAction::SaveAndExit:
      close_game_edit_after_save_ = true;
      game_edit_form_->Save();  // SaveFinished, connected in BuildGameEditCard, closes on success
      return;
    case mira_gui::notify::UnsavedAction::DiscardAndExit:
      CloseGameEdit();
      return;
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
  if (!enabled) ShowHoverCard(QModelIndex());
  // These act on a hidden grid. library_nav_ stays clickable, since it's the way
  // back out. Disabling filter_sort_button_ alone blocks its popover too.
  // The zoom slider is SyncZoom's.
  for (QWidget* control :
       {filter_sort_button_, static_cast<QWidget*>(add_games_), static_cast<QWidget*>(search_),
        static_cast<QWidget*>(settings_button_), static_cast<QWidget*>(fetch_art_button_)}) {
    control->setEnabled(enabled);
  }
  // Back from Settings onto a source page: the grid is still covered.
  if (enabled && (source_page_ != nullptr || runners_page_ != nullptr)) SetSourceControlsEnabled(false);
}

void LibraryWindow::UpdateLibraryNavActive() {
  using mira_gui::icons::Glyph;
  const mira_gui::theme::Tokens& tokens = mira_gui::theme::Current();

  const bool library_active = GridShown() && !GameEditOpen();
  if (library_nav_ != nullptr) {
    library_nav_->setChecked(library_active);
    library_nav_->setIcon(
        mira_gui::icons::For(Glyph::Home, library_active ? tokens.on_accent : tokens.text));
  }
  if (runners_nav_ != nullptr) {
    const bool runners_active = runners_page_ != nullptr && content_stack_->currentWidget() == splitter_;
    runners_nav_->setChecked(runners_active);
    runners_nav_->setIcon(mira_gui::icons::For(Glyph::Wrench, runners_active ? tokens.on_accent : tokens.text));
  }
  // Only the pages it acts on show the tile size.
  if (zoom_ != nullptr) {
    const bool source_shown = content_stack_->currentWidget() == splitter_ && source_page_ != nullptr &&
                              main_stack_->currentWidget() == source_page_;
    zoom_->setVisible(GridShown() || source_shown);
  }
  const QString open_source = content_stack_->currentWidget() == splitter_ && source_page_ != nullptr
                                  ? source_page_->property("source_id").toString()
                                  : QString();
  const std::vector<mira_gui::SourceInfo>& sources = mira_gui::AllSources();
  for (int i = 0; i < source_navs_.size() && i < static_cast<int>(sources.size()); ++i) {
    const bool active = sources[i].id == open_source;
    source_navs_[i]->setChecked(active);
    source_navs_[i]->setIcon(SourceIcon(sources[i], active));
    source_counts_[i]->setStyleSheet(active ? QString("color: %1;").arg(tokens.on_accent.name()) : QString());
  }
  // Every page switch ends here, so the slider follows the page too.
  SyncZoom();
}

QIcon LibraryWindow::SourceIcon(const mira_gui::SourceInfo& source, bool active) const {
  const mira_gui::theme::Tokens& tokens = mira_gui::theme::Current();
  if (!source_icons_) return mira_gui::icons::For(mira_gui::icons::Glyph::Dot, active ? tokens.on_accent : source.color);
  // The source's initial on its color, like its page's header.
  const qreal ratio = devicePixelRatioF();
  constexpr int kSize = 22;
  QPixmap pixmap(QSize(kSize, kSize) * ratio);
  pixmap.setDevicePixelRatio(ratio);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  painter.setBrush(source.color);
  painter.drawRoundedRect(QRectF(0, 0, kSize, kSize), 6, 6);
  QFont font = this->font();
  font.setPixelSize(12);
  font.setWeight(QFont::Bold);
  painter.setFont(font);
  painter.setPen(Qt::white);
  painter.drawText(QRectF(0, 0, kSize, kSize), Qt::AlignCenter, source.name.left(1));
  return QIcon(pixmap);
}

void LibraryWindow::RequestCloseSettings() {
  if (settings_panel_ == nullptr || !settings_panel_->IsDirty()) {
    CloseSettings();
    return;
  }
  switch (mira_gui::notify::ConfirmUnsaved(this, "Settings changed but not saved.")) {
    case mira_gui::notify::UnsavedAction::Cancel:
      return;
    case mira_gui::notify::UnsavedAction::SaveAndExit:
      close_settings_after_save_ = true;
      settings_panel_->Save();  // SaveFinished, connected in BuildSettingsPage, closes on success
      return;
    case mira_gui::notify::UnsavedAction::DiscardAndExit:
      CloseSettings();
      return;
  }
}

QWidget* LibraryWindow::BuildSettingsPage() {
  auto* page = new QWidget(this);
  auto* layout = new QVBoxLayout(page);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  mira_gui::SettingsPanel::Previews previews;
  previews.artwork = artwork_;
  for (const mira_gui::GameSummary* game : PinnedGames()) previews.pinned.push_back(*game);
  for (const mira_gui::GameSummary* game : RecentGames(10)) previews.recent.push_back(*game);
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
            RefreshSourceNavs();
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
      "Move games…", [this] { RelocateLibrary(); });
  settings_panel_->AddSectionAction(
      "Desktop entries", "Menu entries", "Regenerate desktop entries",
      "Rewrites Mira's desktop entries now, so changes to the desktop entry settings apply "
      "without waiting for the next library change.",
      "Regenerate", [this] { SyncDesktopEntries(); });
  settings_panel_->AddSectionAction("Desktop entries", "Menu entries", "Remove all desktop entries",
                                    "Turns off desktop entries and deletes every one Mira generated.",
                                    "Remove…", [this] { RemoveAllDesktopEntries(); });
  return page;
}

QWidget* LibraryWindow::BuildGameEditOverlay() {
  // Parented to nullptr here -- root_stack_->addWidget(overlay) reparents it
  // to central, same as any other widget added to a layout.
  auto* overlay = new ModalOverlay(nullptr);
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
  auto* overlay = new ModalOverlay(nullptr);
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
  sidebar_card_layout_->setContentsMargins(splitter_->widget(0)->width() + kResizeMargin + 24, 24, 24, 24);
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
  auto* card = new mira_gui::SidebarStyleCard({pinned_style_, recent_style_, recent_count_, recent_when_},
                                              copies(PinnedGames()), copies(RecentGames(10)), artwork_);
  connect(card, &mira_gui::SidebarStyleCard::Changed, this, [this](const mira_gui::SidebarStyleCard::Choices& choices) {
    pinned_style_ = choices.pinned;
    recent_style_ = choices.recent;
    recent_count_ = choices.recent_count;
    recent_when_ = choices.recent_when;
    SaveSidebarStyle();
  });
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

void LibraryWindow::SaveSidebarStyle() {
  RefreshSidebarGames();
  mira_gui::FrontendPrefs prefs;
  prefs.sidebar_pinned_style = mira_gui::sidebar::StyleKey(pinned_style_);
  prefs.sidebar_recent_style = mira_gui::sidebar::StyleKey(recent_style_);
  prefs.sidebar_recent_count = recent_count_;
  prefs.sidebar_recent_when = recent_when_;
  // Shown already, so a failed write would otherwise only surface as the old look after a restart.
  mira_gui::MiradClient::SaveFrontendPrefsAsync(this, prefs, [this](mira_gui::PatchConfigResult result) {
    if (!result.ok) mira_gui::notify::FailedRequest(this, "Could not save the sidebar's look.", result.error);
  });
}

// ~70% of the window, following it as it resizes.
void LibraryWindow::SizeGameEditCard(QWidget* card) {
  if (card != nullptr) card->setFixedSize(qRound(width() * 0.7), qRound(height() * 0.7));
}

QWidget* LibraryWindow::BuildGameEditCard(const std::string& id) {
  const mira_gui::theme::Tokens& tokens = mira_gui::theme::Current();
  const mira_gui::GameSummary* game = FindGame(id);

  // The hero fills the card's top and fades into it; everything below sits
  // over it.
  game_edit_backdrop_ = new mira_gui::HeroBackdrop(artwork_);
  QWidget* card = game_edit_backdrop_;
  if (game != nullptr) game_edit_backdrop_->ShowGame(*game);
  SizeGameEditCard(card);

  auto* layout = new QVBoxLayout(card);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  // Back at the top left, as in Settings; the art's one button at the right.
  auto* top = new QHBoxLayout();
  top->setContentsMargins(14, 12, 14, 0);
  // hero_action's text color in base.qss: the buttons are dark glass in every theme.
  const QColor on_glass(0xe6, 0xe8, 0xec);
  auto* back = new QPushButton(card);
  back->setObjectName("hero_action");
  back->setIcon(mira_gui::icons::For(mira_gui::icons::Glyph::ArrowLeft, on_glass));
  back->setToolTip("Back");
  connect(back, &QPushButton::clicked, this, &LibraryWindow::GameEditBack);
  auto* art = new QPushButton("Change art", card);
  art->setObjectName("hero_action");
  art->setIcon(mira_gui::icons::For(mira_gui::icons::Glyph::Image, on_glass));
  art->setCheckable(true);
  // setChecked is OpenArtPicker/CloseArtPicker's, not the click's.
  connect(art, &QPushButton::clicked, this, [this, art] {
    art->setChecked(!art->isChecked());
    if (ArtPickerOpen()) {
      CloseArtPicker();
    } else {
      OpenArtPicker();
    }
  });
  game_edit_art_button_ = art;
  top->addWidget(back);
  top->addStretch(1);
  top->addWidget(art);
  layout->addLayout(top);

  // Name and status over the art. The shadow, in the surface's color, keeps
  // it off the art's detail.
  auto* header = new QHBoxLayout();
  header->setContentsMargins(24, 4, 20, 16);
  header->setSpacing(16);
  auto* identity = new QVBoxLayout();
  identity->setSpacing(4);
  auto* title = new QLabel(game != nullptr ? QString::fromStdString(game->name) : "Game settings", card);
  game_edit_title_ = title;
  title->setObjectName("game_edit_title");
  title->setWordWrap(true);
  auto* shadow = new QGraphicsDropShadowEffect(title);
  shadow->setColor(tokens.surface);
  shadow->setBlurRadius(18);
  shadow->setOffset(0, 1);
  title->setGraphicsEffect(shadow);
  identity->addWidget(title);
  if (game != nullptr) {
    const bool running = game->running;
    const QColor status_color = mira_gui::StatusColor(running ? "running" : game->status);
    QString source = mira_gui::StatusLabel(game->source);
    for (const mira_gui::SourceInfo& info : mira_gui::AllSources()) {
      if (info.id.toStdString() == game->source) source = info.name;
    }
    QStringList facts;
    if (!source.isEmpty()) facts << source;
    if (!game->platform.empty()) facts << mira_gui::StatusLabel(game->platform);
    if (game->play_seconds > 0 && !mira_gui::IsApp(*game)) facts << mira_gui::FormatPlaytime(game->play_seconds) + " played";
    auto* status = new QLabel(
        QString("<span style='color:%1; font-weight:600;'>%2</span>&nbsp;&nbsp;%3")
            .arg(status_color.name(), running ? "Playing" : mira_gui::StatusLabel(game->status),
                 facts.join(" · ").toHtmlEscaped()),
        card);
    status->setTextFormat(Qt::RichText);
    identity->addWidget(status);
  }
  // The cover, which the hero otherwise hides, and where a picked one previews.
  game_edit_cover_ = new mira_gui::CoverChip(artwork_, card);
  if (game != nullptr) game_edit_cover_->ShowGame(*game);
  identity->insertStretch(0, 1);
  auto* play = new QPushButton(card);
  play->setIcon(mira_gui::icons::For(mira_gui::icons::Glyph::Play, tokens.on_accent));
  play->setDefault(true);
  connect(play, &QPushButton::clicked, this, [this, id] { ToggleRunning(id); });
  game_edit_play_ = play;
  // Follows the game starting and stopping; the connections go with the button.
  connect(library_, &QAbstractItemModel::dataChanged, play, [this] { UpdateGameEditPlay(); });
  connect(library_, &QAbstractItemModel::modelReset, play, [this] { UpdateGameEditPlay(); });
  header->addWidget(game_edit_cover_, 0, Qt::AlignBottom);
  header->addLayout(identity, /*stretch=*/1);
  header->addWidget(play, 0, Qt::AlignBottom);
  layout->addLayout(header);

  // Translucent, so the art still shows through at its top edge.
  auto* panel = new QWidget(card);
  panel->setObjectName("game_edit_panel");
  panel->setAttribute(Qt::WA_StyledBackground);
  QColor panel_color = tokens.window;
  panel_color.setAlphaF(0.82);
  panel->setStyleSheet(QString("QWidget#game_edit_panel { background: rgba(%1, %2, %3, %4); border: 1px solid "
                               "%5; border-radius: %6px; }")
                           .arg(panel_color.red())
                           .arg(panel_color.green())
                           .arg(panel_color.blue())
                           .arg(panel_color.alpha())
                           .arg(tokens.border.name())
                           .arg(tokens.radius_panel));
  auto* panel_layout = new QVBoxLayout(panel);
  panel_layout->setContentsMargins(0, 0, 0, 0);
  auto* panel_row = new QHBoxLayout();
  panel_row->setContentsMargins(20, 0, 20, 20);
  panel_row->addWidget(panel);
  layout->addLayout(panel_row, /*stretch=*/1);

  game_edit_stack_ = new QStackedWidget(panel);
  panel_layout->addWidget(game_edit_stack_);
  game_edit_form_ = new mira_gui::GameEditForm(id, game_edit_stack_);
  game_edit_stack_->addWidget(game_edit_form_);
  UpdateGameEditPlay();
  QStringList tags;
  for (const mira_gui::GameSummary& known : library_->Games()) {
    for (const std::string& tag : known.tags) tags << QString::fromStdString(tag);
  }
  tags.removeDuplicates();
  tags.sort(Qt::CaseInsensitive);
  game_edit_form_->SetTagSuggestions(tags);

  game_edit_bar_ = new mira_gui::ChangeBar(card);
  close_game_edit_after_save_ = false;
  // While the art picker is open the bar is its Cancel and Use.
  connect(game_edit_bar_, &mira_gui::ChangeBar::DiscardClicked, this, [this] {
    if (ArtPickerOpen()) return game_edit_picker_->ResetPick();
    game_edit_form_->DiscardChanges();
  });
  connect(game_edit_bar_, &mira_gui::ChangeBar::SaveClicked, this, [this] {
    if (ArtPickerOpen()) {
      game_edit_picker_->Apply();
      CloseArtPicker(/*applied=*/true);
      return;
    }
    game_edit_bar_->SetBusy(true);
    game_edit_form_->Save();
  });
  connect(game_edit_form_, &mira_gui::GameEditForm::Changed, this, [this] {
    if (!ArtPickerOpen()) UpdateGameEditBar();
  });
  connect(game_edit_form_, &mira_gui::GameEditForm::Loaded, title, &QLabel::setText);
  connect(game_edit_form_, &mira_gui::GameEditForm::LoadFailed, this, [this](QString error) {
    mira_gui::notify::Failed(this, "Could not load this game.", error);
    CloseGameEdit();
  });
  connect(game_edit_form_, &mira_gui::GameEditForm::SaveFinished, this,
          [this](bool ok, QString error) {
            const bool close = std::exchange(close_game_edit_after_save_, false);
            game_edit_bar_->SetBusy(false);
            if (!ok) {
              mira_gui::notify::Failed(this, "Could not save this game.", error);
              return;
            }
            // The change bar going away is the feedback; no notice for a save the user just made.
            if (close) CloseGameEdit();
          });

  return card;
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
  connect(source_page_, &mira_gui::SourcePage::LibraryChanged, this, [this, id = source.id] { NoteImported(id); });
  connect(source_page_, &mira_gui::SourcePage::OpenSettingsRequested, this,
          [this](const QString& key) { OpenSettings(key); });
  connect(source_page_, &mira_gui::SourcePage::Removed, this, [this, id = source.id] {
    if (mira_gui::SourceSettingsCard* card = source_page_->SettingsCard()) card->Discard();
    CloseSource();
    ForgetSource(id);
  });
  // Same rule as the grid's double-click: only a ready game has anything to launch.
  connect(source_page_, &mira_gui::SourcePage::GameMenuRequested, this,
          [this](const QString& id, const QPoint& pos, const QString& update_ref) {
            mira_gui::SourcePage* page = source_page_;
            ShowGameMenu(id.toStdString(), pos, [page, update_ref](QMenu& menu) {
              if (update_ref.isEmpty() || page == nullptr) return;
              QObject::connect(menu.addAction("Update"), &QAction::triggered, page,
                               [page, update_ref] { page->UpdateTitle(update_ref); });
            });
          });
  connect(source_page_, &mira_gui::SourcePage::BatchMenuRequested, this,
          [this](const QStringList& ids, const QPoint& pos) {
            std::vector<std::string> games;
            for (const QString& id : ids) games.push_back(id.toStdString());
            ShowBatchMenu(games, pos);
          });
  connect(source_page_, &mira_gui::SourcePage::PlayRequested, this, [this](const QString& id) {
    const mira_gui::GameSummary* game = FindGame(id.toStdString());
    if (game == nullptr) return;
    if (game->running || game->status == "ready") ToggleRunning(game->id);
  });
  main_stack_->addWidget(source_page_);
  main_stack_->setCurrentWidget(source_page_);
  SetSourceControlsEnabled(false);
  UpdateLibraryNavActive();
}

bool LibraryWindow::ConfirmLeaveSource(std::function<void()> retry) {
  mira_gui::SourceSettingsCard* card = source_page_ != nullptr ? source_page_->SettingsCard() : nullptr;
  if (card == nullptr || !card->IsDirty()) return true;
  const mira_gui::notify::UnsavedAction action =
      mira_gui::notify::ConfirmUnsaved(this, "This source's settings changed but aren't saved.");
  // The nav row that was clicked checked itself; staying put unchecks it.
  UpdateLibraryNavActive();
  switch (action) {
    case mira_gui::notify::UnsavedAction::Cancel:
      return false;
    case mira_gui::notify::UnsavedAction::SaveAndExit:
      // A failed save stays on the page, with its error on the card.
      connect(card, &mira_gui::SourceSettingsCard::SaveFinished, this,
              [retry = std::move(retry)](bool ok) {
                if (ok) retry();
              },
              Qt::SingleShotConnection);
      card->Save();
      return false;
    case mira_gui::notify::UnsavedAction::DiscardAndExit:
      return true;
  }
  return true;
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
  RefreshSourceNavs();  // a sign-in or launcher install there changes the order
  return true;
}

// The grid's search and filter leave with its page; the slider is SyncZoom's.
void LibraryWindow::SetSourceControlsEnabled(bool enabled) {
  if (!enabled) ShowHoverCard(QModelIndex());
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
  return "Installing… " + QLocale().formattedDataSize(entry->bytes);
}

void LibraryWindow::DownloadChanged(const QString& key) {
  const int running = downloads_->RunningCount();
  downloads_button_->setText(running > 0 ? QString::number(running) : QString());
  downloads_button_->setToolButtonStyle(running > 0 ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly);
  downloads_button_->setToolTip(running == 0 ? QString("Activity")
                                             : QString("Activity: %1 running").arg(running));

  // That game's row repaints with its new install text.
  if (key.startsWith("game:")) library_->Touch(key.mid(5).toStdString());
}

void LibraryWindow::ShowGame(const std::string& id) {
  if (!LeaveOverlays()) return;
  if (source_page_ != nullptr && !CloseSource([this, id] { ShowGame(id); })) return;
  CloseRunners();
  if (const QModelIndex tile = grid_games_->mapFromSource(library_->IndexOf(id)); tile.isValid()) {
    grid_->setCurrentIndex(tile);  // ClearAndSelect: this game alone
    grid_->scrollTo(tile, QAbstractItemView::PositionAtCenter);
    return;
  }
  OpenGameDialog(id);  // filtered out of the grid: its settings instead
}

void LibraryWindow::RelocateLibrary() {
  if (!mira_gui::notify::Confirm(
          this, "Move Games into Mira's Folders",
          "Move every game's files into your games folder, and each prefix into the prefixes "
          "folder, named after the game? Games installed by a store (Steam, Epic, GOG, itch.io) "
          "keep their install folder; only the prefix moves. Games on another drive are copied "
          "then deleted, which can take a while.",
          "Move games")) {
    return;
  }
  mira_gui::notify::Notice(this, "Moving games into Mira's folders…");
  mira_gui::MiradClient::RelocateLibraryAsync(this, [this](mira_gui::RelocateLibraryResult result) {
    if (!result.ok) {
      mira_gui::notify::FailedRequest(this, "Could not move the games.", result.error);
      return;
    }
    // The moved games arrive as game.updated events.
    if (result.moved > 0 || result.errors.empty()) {
      mira_gui::notify::Notice(this, result.moved == 0 ? QString("Every game was already in place.")
                                                       : QString("Moved %1 game%2.")
                                                             .arg(result.moved)
                                                             .arg(result.moved == 1 ? "" : "s"));
    }
    if (!result.errors.empty()) {
      QStringList failed;
      for (const mira_gui::GameFailure& failure : result.errors) {
        const mira_gui::GameSummary* game = FindGame(failure.id);
        failed << QString::fromStdString(game != nullptr ? game->name : failure.id);
      }
      mira_gui::notify::FailedRequest(this, QString("Could not move %1.").arg(failed.join(", ")),
                                      result.errors.front().error);
    }
  });
}

void LibraryWindow::RefreshSourceNavs() {
  mira_gui::MiradClient::GetConfigAsync(this, [this](mira_gui::ConfigResult result) {
    if (!result.ok) return;
    disabled_sources_.clear();
    for (const mira_gui::SourceInfo& source : mira_gui::AllSources()) {
      const auto found = result.values.find(source.id.toStdString() + ".enabled");
      if (found != result.values.end() && found->second == "false") disabled_sources_.insert(source.id);
    }
    UpdateSourceNavs();
  });
  // A store counts as set up once signed in, a launcher once installed.
  for (const mira_gui::SourceInfo& source : mira_gui::AllSources()) {
    if (source.kind != mira_gui::SourceInfo::Kind::Store) continue;
    const QString id = source.id;
    mira_gui::MiradClient::GetStoreStatusAsync(
        this, id.toStdString(), [this, id](mira_gui::StoreStatusResult status) {
          if (!status.ok) return;  // a failed request says nothing about the account
          source_ready_[id] = status.authenticated;
          source_account_[id] = QString::fromStdString(status.account);
          UpdateSourceNavs();
        });
  }
  mira_gui::MiradClient::GetLaunchersAsync(this, [this](mira_gui::LaunchersResult result) {
    if (!result.ok) return;
    for (const mira_gui::LauncherInfo& launcher : result.launchers) {
      source_ready_[QString::fromStdString(launcher.id)] = launcher.installed;
    }
    UpdateSourceNavs();
  });
}

void LibraryWindow::UpdateSourceNavs() {
  if (source_nav_layout_ == nullptr) return;
  std::map<std::string, int> counts;
  for (const mira_gui::GameSummary& game : library_->Games()) ++counts[game.source];

  const std::vector<mira_gui::SourceInfo>& sources = mira_gui::AllSources();
  for (int i = 0; i < source_navs_.size() && i < static_cast<int>(sources.size()); ++i) {
    const QString& id = sources[i].id;
    const auto count = counts.find(id.toStdString());
    const int games = count == counts.end() ? 0 : count->second;
    const bool ready = games > 0 || source_ready_.value(id, false);
    source_navs_[i]->setVisible(ready && !hidden_sources_.contains(id) && !disabled_sources_.contains(id));
    // A store with games whose account is signed out wants a look.
    const bool signed_out = sources[i].kind == mira_gui::SourceInfo::Kind::Store && games > 0 &&
                            source_ready_.contains(id) && !source_ready_.value(id);
    QString label = show_source_counts_ ? QString::number(games) : QString();
    if (signed_out) label = mira_gui::StatusDot(mira_gui::theme::Current().warning) + label;
    source_counts_[i]->setText(label);
    source_navs_[i]->setToolTip(signed_out ? QString("Signed out of %1").arg(sources[i].name) : QString());
  }
  int row = 0;
  for (const QString& id : SourceOrder()) {
    for (int i = 0; i < static_cast<int>(sources.size()); ++i) {
      if (sources[i].id != id) continue;
      source_nav_layout_->removeWidget(source_navs_[i]);
      source_nav_layout_->insertWidget(row++, source_navs_[i]);
    }
  }
  UpdateLibraryNavActive();
  if (auto* card = qobject_cast<mira_gui::ManageSourcesCard*>(sidebar_card_)) card->SetEntries(SourceEntries());
}

std::vector<mira_gui::ManageSourcesCard::Entry> LibraryWindow::SourceEntries() const {
  std::map<std::string, int> counts;
  for (const mira_gui::GameSummary& game : library_->Games()) ++counts[game.source];
  std::vector<mira_gui::ManageSourcesCard::Entry> entries;
  for (const QString& id : SourceOrder()) {
    const auto source = std::ranges::find(mira_gui::AllSources(), id, &mira_gui::SourceInfo::id);
    const auto count = counts.find(id.toStdString());
    const int games = count == counts.end() ? 0 : count->second;
    entries.push_back({.source = *source,
                       .ready = games > 0 || source_ready_.value(id, false),
                       .enabled = !disabled_sources_.contains(id),
                       .games = games,
                       .in_sidebar = !hidden_sources_.contains(id),
                       .account = source_account_.value(id).toStdString(),
                       .imported_at = source_imported_at_.value(id, 0)});
  }
  return entries;
}

void LibraryWindow::NoteImported(const QString& id) {
  source_imported_at_[id] = QDateTime::currentSecsSinceEpoch();
  mira_gui::FrontendPrefs prefs;
  std::map<std::string, std::int64_t> imported;
  for (auto it = source_imported_at_.cbegin(); it != source_imported_at_.cend(); ++it) {
    imported[it.key().toStdString()] = it.value();
  }
  prefs.source_imported_at = std::move(imported);
  mira_gui::MiradClient::SaveFrontendPrefsAsync(this, prefs, [](mira_gui::PatchConfigResult) {});
}

void LibraryWindow::OpenManageSources() {
  if (!LeaveOverlays()) return;
  auto* card = new mira_gui::ManageSourcesCard();
  card->SetEntries(SourceEntries());
  connect(card, &mira_gui::ManageSourcesCard::CloseRequested, this, &LibraryWindow::CloseSidebarCard);
  connect(card, &mira_gui::ManageSourcesCard::SidebarToggled, this,
          [this](const QString& id, bool shown) { SetSourceHidden(id, !shown); });
  connect(card, &mira_gui::ManageSourcesCard::OrderChanged, this, [this](const QStringList& ids) {
    SetSourceOrder(std::vector<QString>(ids.begin(), ids.end()));
  });
  connect(card, &mira_gui::ManageSourcesCard::EnabledToggled, this, [this](const QString& id, bool on) {
    if (on) {
      disabled_sources_.remove(id);
    } else {
      disabled_sources_.insert(id);
    }
    UpdateSourceNavs();
    const mira_gui::ConfigEdit edit{(id + ".enabled").toStdString(), "a boolean", on ? "true" : "false"};
    mira_gui::MiradClient::PatchConfigAsync(this, {edit}, [this](mira_gui::PatchConfigResult result) {
      if (!result.ok) mira_gui::notify::FailedRequest(this, "Could not change that source.", result.error);
      RefreshSourceNavs();
    });
  });
  connect(card, &mira_gui::ManageSourcesCard::Imported, this, [this](const QString& id) { NoteImported(id); });
  connect(card, &mira_gui::ManageSourcesCard::Removed, this, [this](const QString& id) {
    ForgetSource(id);
    UpdateSourceNavs();
  });
  connect(card, &mira_gui::ManageSourcesCard::OpenRequested, this, [this](const QString& id) {
    CloseSidebarCard();
    if (const mira_gui::SourceInfo* source = mira_gui::FindSourceInfo(id)) OpenSource(*source);
  });
  ShowSidebarCard(card);
}

void LibraryWindow::ForgetSource(const QString& id) {
  disabled_sources_.insert(id);
  source_ready_[id] = false;
  // mirad's game.removed events say the same; this just doesn't wait for them.
  library_->RemoveSource(id.toStdString());
  RefreshSourceNavs();
}

std::vector<QString> LibraryWindow::SourceOrder() const {
  std::vector<QString> order;
  for (const QString& id : source_order_) {
    const bool known = std::ranges::any_of(mira_gui::AllSources(),
                                           [&id](const mira_gui::SourceInfo& source) { return source.id == id; });
    if (known && std::ranges::find(order, id) == order.end()) order.push_back(id);
  }
  for (const mira_gui::SourceInfo& source : mira_gui::AllSources()) {
    if (std::ranges::find(order, source.id) == order.end()) order.push_back(source.id);
  }
  return order;
}

int LibraryWindow::SourceDropRow(int y) const {
  // The layout row of the first visible source whose middle is below `y`.
  int best = -1;
  int best_top = 0;
  for (QPushButton* nav : source_navs_) {
    if (!nav->isVisible() || y >= nav->geometry().center().y()) continue;
    if (best == -1 || nav->geometry().top() < best_top) {
      best = source_nav_layout_->indexOf(nav);
      best_top = nav->geometry().top();
    }
  }
  return best;
}

void LibraryWindow::MoveSource(const QString& id, int before) {
  std::vector<QString> order = SourceOrder();
  QString before_id;
  if (before >= 0) {
    if (auto* nav = qobject_cast<QPushButton*>(source_nav_layout_->itemAt(before)->widget())) {
      before_id = mira_gui::AllSources()[source_navs_.indexOf(nav)].id;
    }
  }
  if (before_id == id) return;
  std::erase(order, id);
  const auto at = before_id.isEmpty() ? order.end() : std::ranges::find(order, before_id);
  order.insert(at, id);
  SetSourceOrder(std::move(order));
}

void LibraryWindow::SetSourceOrder(std::vector<QString> order) {
  source_order_ = order;
  UpdateSourceNavs();
  mira_gui::FrontendPrefs prefs;
  std::vector<std::string> ids;
  for (const QString& source : order) ids.push_back(source.toStdString());
  prefs.source_order = std::move(ids);
  mira_gui::MiradClient::SaveFrontendPrefsAsync(this, prefs, [](mira_gui::PatchConfigResult) {});
}

void LibraryWindow::SetSourceHidden(const QString& id, bool hidden) {
  if (hidden) {
    hidden_sources_.insert(id);
  } else {
    hidden_sources_.remove(id);
  }
  UpdateSourceNavs();
  mira_gui::FrontendPrefs prefs;
  std::vector<std::string> ids;
  for (const QString& hidden_id : hidden_sources_) ids.push_back(hidden_id.toStdString());
  std::ranges::sort(ids);
  prefs.hidden_sources = std::move(ids);
  mira_gui::MiradClient::SaveFrontendPrefsAsync(this, prefs, [](mira_gui::PatchConfigResult) {});
}

std::vector<const mira_gui::GameSummary*> LibraryWindow::PinnedGames() const {
  // By name, matching the grid: hidden pins only under the Hidden filter.
  const bool showing_hidden = CurrentFilterKey() == "hidden";
  std::vector<const mira_gui::GameSummary*> pinned;
  for (const mira_gui::GameSummary& game : library_->Games()) {
    if (HasTag(game, kPinnedTag) && HasTag(game, "hidden") == showing_hidden) pinned.push_back(&game);
  }
  std::ranges::sort(pinned, [](const mira_gui::GameSummary* a, const mira_gui::GameSummary* b) {
    return QString::compare(QString::fromStdString(a->name), QString::fromStdString(b->name),
                            Qt::CaseInsensitive) < 0;
  });
  return pinned;
}

std::vector<const mira_gui::GameSummary*> LibraryWindow::RecentGames(int count, bool running_counts) const {
  // Every running game, then up to `count` others by last played, or with
  // `running_counts` up to `count` in all. A hidden game shows only while it
  // runs, so it can still be stopped.
  std::vector<const mira_gui::GameSummary*> running;
  std::vector<const mira_gui::GameSummary*> played;
  for (const mira_gui::GameSummary& game : library_->Games()) {
    if (game.running) {
      running.push_back(&game);
    } else if (game.last_played_at && !HasTag(game, "hidden")) {
      played.push_back(&game);
    }
  }
  std::ranges::sort(played, [](const mira_gui::GameSummary* a, const mira_gui::GameSummary* b) {
    return *a->last_played_at > *b->last_played_at;
  });
  if (running_counts) count = std::max(0, count - static_cast<int>(running.size()));
  if (played.size() > static_cast<size_t>(count)) played.resize(count);
  played.insert(played.begin(), running.begin(), running.end());
  return played;
}

void LibraryWindow::RefreshSidebarGames() {
  if (recent_layout_ == nullptr) return;
  FillSidebarSection(pinned_heading_, pinned_layout_, PinnedGames(), pinned_style_, /*recent=*/false,
                     pinned_signature_);
  // A shelf keeps the size it was given: running games take places in it.
  FillSidebarSection(recent_heading_, recent_layout_,
                     RecentGames(recent_count_, recent_style_ == mira_gui::sidebar::Style::Shelf), recent_style_,
                     /*recent=*/true, recent_signature_);
}

void LibraryWindow::FillSidebarSection(QWidget* heading, QVBoxLayout* layout,
                                       const std::vector<const mira_gui::GameSummary*>& games,
                                       mira_gui::sidebar::Style style, bool recent, QString& signature) {
  // Most refreshes (every game.updated) change nothing shown here; rebuilding
  // anyway makes the rows flicker.
  // A shelf cover is too narrow for "Yesterday": it gets "1d ago".
  const bool shelf = style == mira_gui::sidebar::Style::Shelf;
  const auto trailing = [recent, shelf, this](const mira_gui::GameSummary& game) {
    if (game.running) return QString("Playing");
    if (!recent || !recent_when_) return QString();
    return shelf ? mira_gui::FormatPlayedAgoShort(game.last_played_at) : mira_gui::FormatPlayedAgo(game.last_played_at);
  };
  QString wanted = mira_gui::theme::Current().running.name() + mira_gui::sidebar::StyleKey(style);
  for (const mira_gui::GameSummary* game : games) {
    wanted += QString("\n%1\t%2\t%3\t%4\t%5\t%6")
                  .arg(QString::fromStdString(game->id), QString::fromStdString(game->name),
                       QString::fromStdString(game->status), game->running ? "1" : "0", trailing(*game),
                       mira_gui::sidebar::ArtSignature(*game, style, artwork_));
  }
  if (wanted == signature) return;
  signature = wanted;

  QWidget* parent = heading->parentWidget();
  parent->setUpdatesEnabled(false);
  // deleteLater: a row's own click or menu may be what got us here. Hidden
  // first: a popup's nested event loop would otherwise keep it painted.
  while (QLayoutItem* item = layout->takeAt(0)) {
    if (QWidget* row = item->widget()) {
      // The hovered row is going away without a Leave, so its card would stay up.
      if (recent_hover_row_ != nullptr && (row == recent_hover_row_ || row->isAncestorOf(recent_hover_row_))) {
        if (recent_hover_ != nullptr) recent_hover_->stop();
        recent_hover_row_ = nullptr;
        ShowHoverCard(QModelIndex());
      }
      row->hide();
      row->deleteLater();
    }
    delete item;
  }
  heading->setVisible(!games.empty());
  using mira_gui::sidebar::Style;
  if (style == Style::Shelf) {
    auto* shelf = new mira_gui::sidebar::Shelf(parent);
    for (const mira_gui::GameSummary* game : games) {
      auto* cover = new mira_gui::sidebar::ShelfCover(*game, artwork_, trailing(*game), shelf);
      WireSidebarGame(cover, *game);
      shelf->Add(cover);
    }
    if (!games.empty()) layout->addWidget(shelf);
    else shelf->deleteLater();
  } else {
    for (const mira_gui::GameSummary* game : games) {
      QPushButton* row = style == Style::Hero
                             ? new mira_gui::sidebar::HeroRow(*game, artwork_, trailing(*game), parent)
                             : mira_gui::sidebar::MakeCoverRow(*game, artwork_, trailing(*game), parent);
      WireSidebarGame(row, *game);
      layout->addWidget(row);
    }
  }
  parent->setUpdatesEnabled(true);
}

void LibraryWindow::WireSidebarGame(QPushButton* row, const mira_gui::GameSummary& game) {
  const std::string id = game.id;
  if (!game.running && game.status == "ready") {
    connect(row, &QPushButton::clicked, this, [this, id] { RowClicked(id); });
  }
  row->setProperty("hover_game", QString::fromStdString(id));
  row->installEventFilter(this);
  row->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(row, &QWidget::customContextMenuRequested, this,
          [this, row, id](const QPoint& pos) { ShowGameMenu(id, row->mapToGlobal(pos)); });
}

void LibraryWindow::RowClicked(const std::string& id) {
  if (last_row_click_.isValid() && last_row_click_.elapsed() < QApplication::doubleClickInterval()) return;
  last_row_click_.start();
  const mira_gui::GameSummary* game = FindGame(id);
  if (game == nullptr) return;
  if (game->running || game->status == "ready") ToggleRunning(id);
}

void LibraryWindow::RefreshContinue() {
  if (continue_row_ == nullptr) return;
  // Only over the whole library: under a filter or a search it's noise.
  std::vector<const mira_gui::GameSummary*> games;
  if (continue_row_enabled_ && CurrentFilterKey() == "all" && search_->text().trimmed().isEmpty()) {
    for (const mira_gui::GameSummary& game : library_->Games()) {
      if (HasTag(game, "hidden") || mira_gui::IsApp(game) || game.source == "launcher") continue;
      if (game.running || game.last_played_at) games.push_back(&game);
    }
    const size_t keep = std::min(games.size(), static_cast<size_t>(continue_count_));
    std::partial_sort(games.begin(), games.begin() + keep, games.end(),
                      [](const mira_gui::GameSummary* a, const mira_gui::GameSummary* b) {
                        if (a->running != b->running) return a->running;
                        return a->last_played_at.value_or(0) > b->last_played_at.value_or(0);
                      });
    games.resize(keep);
  }
  continue_row_->SetGames(games);
}

void LibraryWindow::ShowSourceMenu(const mira_gui::SourceInfo& source, const QPoint& global_pos) {
  QMenu menu(this);
  QAction* open = menu.addAction("Open " + source.name);
  // Neighbours among the visible rows, in their shown order.
  std::vector<QPushButton*> shown;
  for (int row = 0; row < source_nav_layout_->count(); ++row) {
    auto* nav = qobject_cast<QPushButton*>(source_nav_layout_->itemAt(row)->widget());
    if (nav != nullptr && nav->isVisible()) shown.push_back(nav);
  }
  QPushButton* self = source_navs_[std::ranges::find(mira_gui::AllSources(), source.id, &mira_gui::SourceInfo::id) -
                                   mira_gui::AllSources().begin()];
  const auto position = std::ranges::find(shown, self);
  QAction* up = menu.addAction("Move up");
  up->setEnabled(position != shown.end() && position != shown.begin());
  QAction* down = menu.addAction("Move down");
  down->setEnabled(position != shown.end() && position + 1 != shown.end());
  QAction* hide = menu.addAction("Hide from sidebar");
  menu.addSeparator();
  QAction* manage = menu.addAction("Manage sources…");
  QAction* settings = menu.addAction("Sidebar settings…");
  QAction* chosen = menu.exec(global_pos);
  if (chosen == open) {
    OpenSource(source);
  } else if (chosen == up) {
    MoveSource(source.id, source_nav_layout_->indexOf(*(position - 1)));
  } else if (chosen == down) {
    MoveSource(source.id, position + 2 == shown.end() ? -1 : source_nav_layout_->indexOf(*(position + 2)));
  } else if (chosen == hide) {
    SetSourceHidden(source.id, true);
  } else if (chosen == manage) {
    OpenManageSources();
  } else if (chosen == settings) {
    OpenSettings(mira_gui::SettingsPanel::kSidebarKey);
  }
}

void LibraryWindow::ShowSidebarMenu(const QPoint& global_pos) {
  QMenu menu(this);
  QAction* manage = menu.addAction("Manage sources…");
  QAction* customize = menu.addAction("Customize pinned and recent…");
  QAction* settings = menu.addAction("Sidebar settings…");
  QAction* chosen = menu.exec(global_pos);
  if (chosen == manage) {
    OpenManageSources();
  } else if (chosen == customize) {
    OpenSidebarStyle();
  } else if (chosen == settings) {
    OpenSettings(mira_gui::SettingsPanel::kSidebarKey);
  }
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
  if (type == "notification") {
    mira_gui::NotificationEvent event;
    if (live && mira_gui::MiradClient::ParseNotification(data, &event)) {
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

  if (mira_gui::StoreEvent art; mira_gui::MiradClient::ParseTitleArtworkEvent(type, data, &art)) {
    if (art.state == "ready") artwork_->TitleArtworkReady(art.source + "-" + art.ref);
    return;
  }

  if (mira_gui::InstallEvent install; mira_gui::MiradClient::ParseInstallEvent(type, data, &install)) {
    const mira_gui::GameSummary* game = FindGame(install.id);
    const QString name = game != nullptr ? QString::fromStdString(game->name) : QString("A game");
    if (!live) {
      // History: the grid below still picks up the result.
    } else if (install.state == "failed") {
      mira_gui::notify::FailedRequest(this, "Could not install " + name + ".", install.error);
    } else if (install.state == "finished") {
      mira_gui::notify::Notice(this, name + " is installed.");
    }
    // The tile's install text follows the tracker (DownloadChanged); the
    // record itself arrives as game.updated.
    return;
  }

  if (type == "game.removed") {
    const std::string id = mira_gui::MiradClient::ParseRemovedId(data);
    if (!id.empty()) RemoveGame(id);
    return;
  }
  if (type == "games.removed") {
    const std::vector<std::string> ids = mira_gui::MiradClient::ParseRemovedIds(data);
    if (!ids.empty()) library_->Remove(ids);
    return;
  }

  if (type == "game.state") {
    mira_gui::GameStateEvent state;
    if (!mira_gui::MiradClient::ParseGameState(data, &state)) return;
    // The full record, `running` included; a bare {id, state} only moves running.
    if (mira_gui::GameSummary game; mira_gui::MiradClient::ParseGameSummary(data, &game) && !game.name.empty()) {
      game.running = state.state == "running";
      UpsertGames({game});
    } else {
      library_->SetRunning(state.id, state.state == "running");
    }
    // A crash soon after launch is a game that failed to start. A later one is
    // left to the game's status: plenty of games exit non-zero on a normal quit.
    if (live && state.state == "crashed" && state.played_seconds < kFailedStartSeconds) {
      const mira_gui::GameSummary* crashed = FindGame(state.id);
      const QString name = crashed != nullptr ? QString::fromStdString(crashed->name) : QString("The game");
      mira_gui::notify::FailedRequest(this, name + " closed right after starting.", state.error);
    }
    return;
  }

  if (type == "game.metadata_ready" || type == "game.metadata_failed") {
    // History's outcomes are already in what the grid fetched at startup.
    mira_gui::MetadataEvent event;
    if (!live || !mira_gui::MiradClient::ParseMetadataEvent(data, &event)) return;
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
    if (!live || !mira_gui::MiradClient::ParseArtworkSelectEvent(data, &event)) return;
    if (event.slot == "cover") {
      artwork_->NoteArt(event.id, event.art);
    } else if (event.slot == "hero") {
      if (game_edit_backdrop_ != nullptr) game_edit_backdrop_->RefreshHero(event.id);
    }
    return;
  }

  if (type == "game.installer_leftover") {
    // Asked once, as it happens; history would ask again after every reconnect.
    mira_gui::InstallerLeftoverEvent event;
    if (live && mira_gui::MiradClient::ParseInstallerLeftover(data, &event)) OfferInstallerDelete(event);
    return;
  }

  if (type == "game.install_detected") {
    // History's would ask again after every reconnect.
    mira_gui::InstallDetectedEvent event;
    if (!live || !mira_gui::MiradClient::ParseInstallDetected(data, &event)) return;
    QTimer::singleShot(0, this, [this, event] { AskAboutInstall(event); });
    return;
  }

  if (type == "game.launched") {
    // mirad hands a Steam game to steam://rungameid and says whether it is
    // watching the process. Tracked: leave it alone, game.state is coming.
    // Untracked: clear it, since nothing will ever say it stopped.
    mira_gui::GameLaunchedEvent launched;
    if (mira_gui::MiradClient::ParseGameLaunched(data, &launched) && !launched.tracked) {
      library_->SetRunning(launched.id, false);
    }
    return;
  }

  // Explicitly the two event types that carry a game record, not "anything
  // left over": mirad also publishes runners.download.* and tricks.* here.
  if (type == "games.updated") {
    std::vector<mira_gui::GameSummary> games;
    if (mira_gui::MiradClient::ParseGameSummaries(data, &games)) UpsertGames(games);
    return;
  }
  if (type != "game.added" && type != "game.updated") return;

  mira_gui::GameSummary game;
  if (mira_gui::MiradClient::ParseGameSummary(data, &game)) UpsertGames({game});

  // A new installer asks to be run instead of opening its settings, unless mirad already runs it.
  if (live && type == "game.added" && game.status == "needs_install" && !game.id.empty()) {
    if (!mira_gui::MiradClient::ParseAutoInstall(data)) OfferInstall(game.id);
    return;
  }

  // open_config_on_add: open a newly detected game's settings to check them.
  // Only for a lone arrival; a scan that finds several opens nothing rather
  // than stacking cards. Never for history, which would open one at startup.
  if (live && type == "game.added" && mira_gui::MiradClient::ParseOpenConfig(data) && !game.id.empty()) {
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
