#include "AddedSources.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QSet>
#include <QToolButton>
#include <QVBoxLayout>

#include "../client/api/Library.h"
#include "../client/api/Stores.h"
#include "../settings/SettingsCard.h"
#include "../theme/Icons.h"
#include "../widgets/Labels.h"
#include "../widgets/TileGridLayout.h"
#include "SourceRemoval.h"
#include "SourceText.h"
#include "SourceTile.h"

namespace mira_gui {

namespace {

constexpr int kTileWidth = 280;

QString Ago(std::int64_t unix_seconds) {
  const qint64 seconds = QDateTime::currentSecsSinceEpoch() - unix_seconds;
  if (seconds < 60) return "just now";
  if (seconds < 3600) return QString("%1 min ago").arg(seconds / 60);
  if (seconds < 86400) return QString("%1 h ago").arg(seconds / 3600);
  return QString("%1 days ago").arg(seconds / 86400);
}

QString Counted(int n, const QString& item) { return QString("%1 %2%3").arg(n).arg(item).arg(n == 1 ? "" : "s"); }

QString Status(const SourceEntry& entry) {
  const QString items = Counted(entry.games, CopyFor(entry.source.id.toStdString()).item);
  if (!entry.enabled) return "Off · " + items + " kept out of the library";
  QStringList details{items};
  if (!entry.account.empty()) details << "signed in as " + QString::fromStdString(entry.account);
  if (entry.imported_at > 0) details << "imported " + Ago(entry.imported_at);
  return details.join(" · ");
}

// "<text> ........ [control]", one line of a tile's body.
void AddLine(SourceTile* tile, const QString& text, const QString& tip, QWidget* control) {
  auto* line = new QHBoxLayout();
  line->setContentsMargins(0, 0, 4, 0);
  QLabel* label = MakeLabel(tile, text, nullptr, false);
  label->setToolTip(tip);
  control->setToolTip(tip);
  line->addWidget(label, 1);
  line->addWidget(control);
  tile->Body()->addLayout(line);
}

QWidget* Grid(QWidget* parent) {
  auto* grid = new QWidget(parent);
  new TileGridLayout(grid, kTileWidth);
  return grid;
}

}  // namespace

AddedSources::AddedSources(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);
  games_heading_ = MakeGroupHeading(this, "Games");
  games_grid_ = Grid(this);
  apps_heading_ = MakeGroupHeading(this, "Apps");
  apps_heading_->setContentsMargins(0, 12, 0, 0);
  apps_grid_ = Grid(this);
  empty_ = MakeLabel(this, QString(), "subtle");
  empty_->hide();
  for (QWidget* widget : {static_cast<QWidget*>(games_heading_), games_grid_, static_cast<QWidget*>(apps_heading_),
                          apps_grid_, static_cast<QWidget*>(empty_)}) {
    layout->addWidget(widget);
  }
  layout->addStretch(1);
}

void AddedSources::SetEntries(const std::vector<SourceEntry>& entries) {
  QSet<QString> ids;
  for (const SourceEntry& entry : entries) ids.insert(entry.source.id);
  for (auto it = tiles_.begin(); it != tiles_.end();) {
    if (ids.contains(it->first)) {
      ++it;
    } else {
      it->second.tile->deleteLater();
      it = tiles_.erase(it);
    }
  }
  QList<QWidget*> order;
  for (const SourceEntry& entry : entries) {
    Tile& tile = tiles_.contains(entry.source.id) ? tiles_[entry.source.id] : Build(entry);
    tile.entry = entry;
    Update(tile);
    order << tile.tile;
  }
  // The sidebar can be dragged while this is open, so the order follows it.
  for (QWidget* grid : {games_grid_, apps_grid_}) static_cast<TileGridLayout*>(grid->layout())->Reorder(order);
  ApplyFilter();
}

void AddedSources::SetFilter(const QString& text) {
  filter_ = text.trimmed().toLower();
  ApplyFilter();
}

AddedSources::Tile& AddedSources::Build(const SourceEntry& entry) {
  const QString id = entry.source.id;
  const QString name = entry.source.name;
  const bool app = CopyFor(id.toStdString()).item == "app";
  QWidget* grid = app ? apps_grid_ : games_grid_;
  Tile& tile = tiles_[id];
  tile.tile = new SourceTile(entry.source, grid);
  tile.tile->SetClickable("Open " + name);
  connect(tile.tile, &SourceTile::Clicked, this, [this, id] { emit OpenRequested(id); });

  auto* settings = new QToolButton(tile.tile);
  settings->setAutoRaise(true);
  icons::Follow(settings, icons::Glyph::Settings);
  settings->setToolTip(name + " settings");
  settings->setAccessibleName(name + " settings");
  connect(settings, &QToolButton::clicked, this, [this, id] { emit SettingsRequested(id); });
  tile.tile->Corner()->addWidget(settings);
  auto* more = new QToolButton(tile.tile);
  more->setAutoRaise(true);
  icons::Follow(more, icons::Glyph::More);
  more->setToolTip("More for " + name);
  more->setAccessibleName("More for " + name);
  connect(more, &QToolButton::clicked, this, [this, id] { ShowMenu(id); });
  tile.tile->Corner()->addWidget(more);
  tile.more = more;

  tile.tile->Body()->addWidget(MakeDivider(tile.tile, Qt::Horizontal));
  const QString items = app ? "apps" : "games";
  if (id == "local") {
    AddLine(tile.tile, QString("%1 in library").arg(items[0].toUpper() + items.mid(1)),
            "Games from no store or launcher are always in the library.", MakeLabel(tile.tile, "Always", "subtle", false));
  } else {
    tile.in_library = new Switch(tile.tile);
    tile.in_library->setAccessibleName(QString("%1's %2 in the library").arg(name, items));
    AddLine(tile.tile, QString("%1 in library").arg(items[0].toUpper() + items.mid(1)),
            QString("Off keeps %1's %2 out of the library and stops importing. Nothing is deleted.").arg(name, items),
            tile.in_library);
    connect(tile.in_library, &Switch::toggled, this, [this, id](bool on) { emit EnabledToggled(id, on); });
  }
  tile.in_sidebar = new Switch(tile.tile);
  tile.in_sidebar->setAccessibleName(name + " in the sidebar");
  AddLine(tile.tile, "Shown in sidebar", QString("Lists %1 under Sources in the sidebar.").arg(name), tile.in_sidebar);
  connect(tile.in_sidebar, &Switch::toggled, this, [this, id](bool shown) { emit SidebarToggled(id, shown); });

  grid->layout()->addWidget(tile.tile);
  return tile;
}

void AddedSources::Update(Tile& tile) {
  const SourceEntry& entry = tile.entry;
  tile.tile->SetDim(!entry.enabled);
  if (!tile.importing && tile.note.isEmpty()) tile.tile->Line()->setText(Status(entry));
  for (auto [toggle, on] : {std::pair{tile.in_library, entry.enabled}, std::pair{tile.in_sidebar, entry.in_sidebar}}) {
    if (toggle == nullptr) continue;
    toggle->blockSignals(true);
    toggle->setChecked(on);
    toggle->blockSignals(false);
  }
}

void AddedSources::ApplyFilter() {
  bool games_shown = false;
  bool apps_shown = false;
  for (const auto& [id, tile] : tiles_) {
    const SourceInfo& source = tile.entry.source;
    const bool shown = filter_.isEmpty() || source.name.toLower().contains(filter_) ||
                       KindLabel(source.kind).toLower().contains(filter_);
    tile.tile->setVisible(shown);
    (tile.tile->parentWidget() == apps_grid_ ? apps_shown : games_shown) |= shown;
  }
  games_heading_->setVisible(games_shown && apps_shown);  // one group needs no heading
  games_grid_->setVisible(games_shown);
  apps_heading_->setVisible(apps_shown && games_shown);
  apps_grid_->setVisible(apps_shown);
  empty_->setVisible(!games_shown && !apps_shown);
  empty_->setText(filter_.isEmpty() ? "Nothing is set up yet. Add a source to bring its games in."
                                    : "No added source matches that. Add source may have it.");
}

void AddedSources::ShowMenu(const QString& id) {
  if (!tiles_.contains(id)) return;
  const Tile& tile = tiles_[id];
  const SourceEntry entry = tile.entry;
  QMenu menu(this);
  menu.setToolTipsVisible(true);
  if (id != "humble" && id != "local") {
    QAction* import = menu.addAction("Import games now", this, [this, id] { Import(id); });
    import->setEnabled(entry.enabled && !tile.importing);
    if (!entry.enabled) import->setToolTip("Turn on Games in library first.");
  }
  // Local is where games from no source go, so it can't be removed.
  if (id != "local") {
    if (!menu.isEmpty()) menu.addSeparator();
    QAction* remove = menu.addAction("Remove " + entry.source.name + "…", this, [this, entry] {
      RemoveSource(this, entry.source, [this, id = entry.source.id] { emit Removed(id); });
    });
    remove->setToolTip("Undo its setup. It goes back to Add source.");
  }
  if (menu.isEmpty()) return;
  menu.exec(tile.more->mapToGlobal(QPoint(0, tile.more->height())));
}

void AddedSources::Import(const QString& id) {
  if (!tiles_.contains(id)) return;
  Tile& tile = tiles_[id];
  tile.importing = true;
  tile.tile->Line()->setText("Importing…");
  const auto done = [this, id](bool ok, const std::string& error, int added, int updated) {
    if (!tiles_.contains(id)) return;
    Tile& tile = tiles_[id];
    tile.importing = false;
    tile.note = ok ? QString("Imported: %1 added, %2 updated").arg(added).arg(updated)
                   : "Could not import: " + QString::fromStdString(error);
    tile.tile->Line()->setText(tile.note);
    if (ok) emit Imported(id);
  };
  const std::string source = id.toStdString();
  if (source == "steam") {
    api::ScanSteamAsync(this, [done](SteamScanResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else if (source == "lutris") {
    api::ImportLutrisAsync(this, [done](LutrisImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else if (tile.entry.source.kind == SourceInfo::Kind::Launcher) {
    api::ImportLauncherAsync(this, source, [done](StoreImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else {
    api::ImportStoreAsync(this, source, [done](StoreImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  }
}

}  // namespace mira_gui
