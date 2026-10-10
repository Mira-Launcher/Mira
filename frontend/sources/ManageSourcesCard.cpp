#include "ManageSourcesCard.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QMenu>
#include <QStyle>
#include <QToolButton>

#include <QSet>

#include <algorithm>

#include "../client/api/Library.h"
#include "../client/api/Stores.h"
#include "../theme/Icons.h"
#include "../widgets/Labels.h"
#include "SourceRemoval.h"

namespace mira_gui {

namespace {

QString Ago(std::int64_t unix_seconds) {
  const qint64 seconds = QDateTime::currentSecsSinceEpoch() - unix_seconds;
  if (seconds < 60) return "just now";
  if (seconds < 3600) return QString("%1 min ago").arg(seconds / 60);
  if (seconds < 86400) return QString("%1 h ago").arg(seconds / 3600);
  return QString("%1 days ago").arg(seconds / 86400);
}

QString Status(const ManageSourcesCard::Entry& entry) {
  if (!entry.enabled) return "Off";
  QStringList details{QString("%1 game%2").arg(entry.games).arg(entry.games == 1 ? "" : "s")};
  if (!entry.account.empty()) details << "signed in as " + QString::fromStdString(entry.account);
  if (entry.imported_at > 0) details << "imported " + Ago(entry.imported_at);
  return details.join(" · ");
}

}  // namespace

ManageSourcesCard::ManageSourcesCard(const QString& title, QWidget* parent) : SettingsCard(title, parent) {
  connect(this, &SettingsCard::RowsReordered, this, [this] {
    QStringList ids;
    for (QWidget* widget : Rows()) {
      const auto it = std::ranges::find(rows_, widget, [](const Row& row) -> QWidget* { return row.row; });
      if (it != rows_.end()) ids << it->entry.source.id;
    }
    emit OrderChanged(ids);
  });
}

void ManageSourcesCard::SetEntries(const std::vector<Entry>& entries) {
  QSet<QString> ids;
  QSet<QString> have;
  for (const Entry& entry : entries) ids.insert(entry.source.id);
  for (const Row& row : rows_) have.insert(row.entry.source.id);
  if (ids != have) {
    ClearRows();
    rows_.clear();
    for (const Entry& entry : entries) BuildRow(entry);
    return;
  }
  // The order too, as the sidebar beside the card can still be dragged.
  for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
    if (Row* row = Find(entries[i].source.id)) {
      row->entry = entries[i];
      Update(*row);
      MoveRow(row->row, i);
    }
  }
}

void ManageSourcesCard::BuildRow(const Entry& entry) {
  const QString id = entry.source.id;
  Row& row = rows_.emplace_back();
  row.entry = entry;
  row.row = new SettingRow(entry.source.name, {}, this);
  row.row->ShowGrip();
  row.badge = MakeSourceBadge(entry.source, 24, row.row);
  row.row->SetLeading(row.badge);
  row.row->AddAfterLabel(MakeKindTag(entry.source, row.row));
  // Elided, so an account name or import error can't push the controls out of the card.
  row.status = new ElidedLabel(QString(), row.row);
  row.status->setProperty("role", "subtle");
  row.row->AddAfterLabel(row.status);

  row.in_sidebar = new Switch(row.row);
  row.in_sidebar->setAccessibleName(QString("%1 in the sidebar").arg(entry.source.name));
  row.in_sidebar->setToolTip("Show in the sidebar");
  connect(row.in_sidebar, &Switch::toggled, this, [this, id](bool shown) { emit SidebarToggled(id, shown); });
  row.row->AddControl(row.in_sidebar);

  auto* settings = new QToolButton(row.row);
  settings->setAutoRaise(true);
  icons::Follow(settings, icons::Glyph::Settings);
  settings->setToolTip(entry.source.name + " settings");
  connect(settings, &QToolButton::clicked, this, [this, id] { emit SettingsRequested(id); });
  row.row->AddControl(settings);

  row.more = new QToolButton(row.row);
  row.more->setAutoRaise(true);
  icons::Follow(row.more, icons::Glyph::More);
  row.more->setToolTip("More");
  connect(row.more, &QToolButton::clicked, this, [this, id] { ShowMenu(id); });
  row.row->AddControl(row.more);

  // A click anywhere but the grip and controls opens the source's page.
  row.row->setCursor(Qt::PointingHandCursor);
  row.row->installEventFilter(this);
  AddRow(row.row);
}

bool ManageSourcesCard::eventFilter(QObject* watched, QEvent* event) {
  const auto it = std::ranges::find(rows_, watched, [](const Row& row) -> QObject* { return row.row; });
  if (it != rows_.end() && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)) {
    const auto* mouse = static_cast<QMouseEvent*>(event);
    if (event->type() == QEvent::MouseButtonPress) {
      pressed_at_ = mouse->globalPosition().toPoint();
    } else if (mouse->button() == Qt::LeftButton &&
               (mouse->globalPosition().toPoint() - pressed_at_).manhattanLength() < 6) {
      emit OpenRequested(it->entry.source.id);  // a click, not the end of a drag
    }
  }
  return SettingsCard::eventFilter(watched, event);
}

void ManageSourcesCard::Update(Row& row) {
  const Entry& entry = row.entry;
  SetSourceBadgeDim(row.badge, entry.source, !entry.ready || !entry.enabled);
  row.row->Label()->setProperty("role", entry.enabled ? "" : "subtle");
  row.row->Label()->style()->unpolish(row.row->Label());
  row.row->Label()->style()->polish(row.row->Label());
  if (!row.importing && row.note.isEmpty()) row.status->setText(Status(entry));
  row.in_sidebar->blockSignals(true);
  row.in_sidebar->setChecked(entry.in_sidebar);
  row.in_sidebar->blockSignals(false);
  row.in_sidebar->setEnabled(entry.enabled);  // an off source isn't in the sidebar either way
}

void ManageSourcesCard::ShowMenu(const QString& id) {
  Row* row = Find(id);
  if (row == nullptr) return;
  const Entry entry = row->entry;
  QMenu menu(this);
  if (entry.enabled && id != "humble" && id != "local" && !row->importing) {
    menu.addAction("Import games", this, [this, id] { Import(id); });
  }
  // Local is where games from no source go, so it can't be turned off or removed.
  if (id != "local") {
    menu.addAction(entry.enabled ? "Turn off" : "Turn on", this,
                   [this, id, on = !entry.enabled] { emit EnabledToggled(id, on); });
    menu.addSeparator();
    menu.addAction("Remove…", this, [this, entry] {
      RemoveSource(this, entry.source, [this, id = entry.source.id] { emit Removed(id); });
    });
  }
  menu.exec(row->more->mapToGlobal(QPoint(0, row->more->height())));
}

void ManageSourcesCard::Import(const QString& id) {
  Row* row = Find(id);
  if (row == nullptr) return;
  row->importing = true;
  row->status->setText("Importing…");
  const auto done = [this, id](bool ok, const std::string& error, int added, int updated) {
    Row* row = Find(id);
    if (row == nullptr) return;
    row->importing = false;
    row->note = ok ? QString("Imported: %1 added, %2 updated").arg(added).arg(updated)
                   : "Could not import: " + QString::fromStdString(error);
    row->status->setText(row->note);
    if (ok) emit Imported(id);
  };
  const std::string source = id.toStdString();
  if (source == "steam") {
    api::ScanSteamAsync(this, [done](SteamScanResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else if (source == "lutris") {
    api::ImportLutrisAsync(this, [done](LutrisImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else if (row->entry.source.kind == SourceInfo::Kind::Launcher) {
    api::ImportLauncherAsync(this, source,
                                     [done](StoreImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  } else {
    api::ImportStoreAsync(this, source,
                                  [done](StoreImportResult r) { done(r.ok, r.error, r.added, r.updated); });
  }
}

ManageSourcesCard::Row* ManageSourcesCard::Find(const QString& id) {
  const auto it = std::ranges::find(rows_, id, [](const Row& row) { return row.entry.source.id; });
  return it == rows_.end() ? nullptr : &*it;
}

}  // namespace mira_gui
