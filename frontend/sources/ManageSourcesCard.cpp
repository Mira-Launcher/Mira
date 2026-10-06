#include "ManageSourcesCard.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QToolButton>

#include <algorithm>

#include "../client/api/Library.h"
#include "../client/api/Stores.h"
#include "../theme/Icons.h"
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
  if (!entry.ready) return "Not set up";
  QStringList details{QString("%1 game%2").arg(entry.games).arg(entry.games == 1 ? "" : "s")};
  if (!entry.account.empty()) details << "signed in as " + QString::fromStdString(entry.account);
  if (entry.imported_at > 0) details << "imported " + Ago(entry.imported_at);
  if (!entry.in_sidebar) details << "hidden from sidebar";
  return details.join(" · ");
}

}  // namespace

ManageSourcesCard::ManageSourcesCard(QWidget* parent) : SettingsCard("Manage sources", parent) {
  SetProminentTitle();
  setFixedWidth(720);
  auto* close = new QToolButton(this);
  close->setAutoRaise(true);
  icons::Follow(close, icons::Glyph::Close);
  close->setToolTip("Close");
  connect(close, &QToolButton::clicked, this, &ManageSourcesCard::CloseRequested);
  Header()->addWidget(close);

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
  if (rows_.empty()) {
    for (const Entry& entry : entries) BuildRow(entry);
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

  row.set_up = new QPushButton("Set up", row.row);
  connect(row.set_up, &QPushButton::clicked, this, [this, id] { emit OpenRequested(id); });
  QSizePolicy keep = row.set_up->sizePolicy();
  keep.setRetainSizeWhenHidden(true);
  row.set_up->setSizePolicy(keep);
  row.row->AddControl(row.set_up);

  row.enabled = new Switch(row.row);
  row.enabled->setAccessibleName(QString("%1 on").arg(entry.source.name));
  row.enabled->setToolTip("Off hides the source everywhere and stops its imports");
  connect(row.enabled, &Switch::toggled, this, [this, id](bool on) { emit EnabledToggled(id, on); });
  row.row->AddControl(row.enabled);

  row.more = new QToolButton(row.row);
  row.more->setAutoRaise(true);
  icons::Follow(row.more, icons::Glyph::More);
  row.more->setToolTip("More");
  row.more->setSizePolicy(keep);
  connect(row.more, &QToolButton::clicked, this, [this, id] { ShowMenu(id); });
  row.row->AddControl(row.more);

  AddRow(row.row);
}

void ManageSourcesCard::Update(Row& row) {
  const Entry& entry = row.entry;
  SetSourceBadgeDim(row.badge, entry.source, !entry.ready || !entry.enabled);
  if (!row.importing && row.note.isEmpty()) row.status->setText(Status(entry));
  row.enabled->blockSignals(true);
  row.enabled->setChecked(entry.enabled);
  row.enabled->blockSignals(false);
  row.set_up->setVisible(!entry.ready && entry.enabled);
  row.more->setVisible(entry.ready);
}

void ManageSourcesCard::ShowMenu(const QString& id) {
  Row* row = Find(id);
  if (row == nullptr) return;
  const Entry entry = row->entry;
  QMenu menu(this);
  if (entry.enabled) {
    menu.addAction("Open", this, [this, id] { emit OpenRequested(id); });
    if (id != "humble" && !row->importing) menu.addAction("Import games", this, [this, id] { Import(id); });
    QAction* sidebar = menu.addAction("Show in sidebar");
    sidebar->setCheckable(true);
    sidebar->setChecked(entry.in_sidebar);
    connect(sidebar, &QAction::toggled, this, [this, id](bool shown) { emit SidebarToggled(id, shown); });
    menu.addSeparator();
  }
  menu.addAction("Remove…", this, [this, entry] {
    RemoveSource(this, entry.source, [this, id = entry.source.id] { emit Removed(id); });
  });
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
