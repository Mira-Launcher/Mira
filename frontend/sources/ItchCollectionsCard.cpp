#include "ItchCollectionsCard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include "../app/ErrorHelp.h"
#include "../client/api/Stores.h"
#include "../theme/Theme.h"

namespace mira_gui {

ItchCollectionsCard::ItchCollectionsCard(QWidget* parent) : SettingsCard("itch.io collections", parent) {
  auto* intro = new QLabel(
      "Games in these collections show on the itch page. Free ones can be installed; paid ones "
      "install once you own them.",
      this);
  intro->setProperty("role", "muted");
  intro->setWordWrap(true);
  intro->setContentsMargins(18, 8, 18, 8);
  AddRow(intro);

  status_ = new QLabel(this);
  status_->setWordWrap(true);
  status_->setContentsMargins(18, 8, 18, 8);
  AddRow(status_);

  auto* add_row = new SettingRow("Add by link", "A collection's itch.io address.", this);
  link_ = new QLineEdit(add_row);
  link_->setPlaceholderText("https://itch.io/c/123456/collection-name");
  link_->setMinimumWidth(240);
  connect(link_, &QLineEdit::returnPressed, this, &ItchCollectionsCard::Add);
  add_row->AddControl(link_, /*stretch=*/1);
  add_ = new QPushButton("Add", add_row);
  connect(add_, &QPushButton::clicked, this, &ItchCollectionsCard::Add);
  add_row->AddControl(add_);
  AddRow(add_row);
}

void ItchCollectionsCard::Refresh() {
  SetStatus("Loading…");
  api::GetItchCollectionsAsync(this, [this](ItchCollectionsResult result) { ShowCollections(result); });
}

void ItchCollectionsCard::SetStatus(const QString& text, bool error) {
  status_->setText(text);
  status_->setVisible(!text.isEmpty());
  theme::SetStyleProperty(status_, "role", error ? "error" : "muted");
}

void ItchCollectionsCard::ShowCollections(const ItchCollectionsResult& result) {
  for (QWidget* row : collection_rows_) RemoveRow(row);
  collection_rows_.clear();
  if (!result.ok) {
    SetStatus("Could not list the collections: " + error_help::Describe(result.error), true);
    return;
  }
  SetStatus(result.collections.empty() ? "No collections yet." : QString());
  for (const ItchCollection& collection : result.collections) {
    auto* row = new SettingRow(QString::fromStdString(collection.title), QString(), this);
    auto* count = new QLabel(QString("%1 game%2").arg(collection.games_count).arg(collection.games_count == 1 ? "" : "s"), row);
    count->setProperty("role", "muted");
    row->AddControl(count);
    if (collection.own) {
      auto* yours = new QLabel("Yours", row);
      yours->setProperty("role", "muted");
      row->AddControl(yours);
    } else {
      auto* remove = new QPushButton("Remove", row);
      const std::int64_t id = collection.id;
      connect(remove, &QPushButton::clicked, this, [this, id] {
        api::RemoveItchCollectionAsync(this, id, [this](StoreActionResult r) {
          if (!r.ok) return SetStatus("Could not remove it: " + error_help::Describe(r.error), true);
          emit Changed();
          Refresh();
        });
      });
      row->AddControl(remove);
    }
    AddRow(row);
    MoveRow(row, static_cast<int>(1 + collection_rows_.size()));
    collection_rows_.push_back(row);
  }
}

void ItchCollectionsCard::Add() {
  const QString link = link_->text().trimmed();
  if (link.isEmpty()) return;
  add_->setEnabled(false);
  SetStatus("Adding…");
  api::AddItchCollectionAsync(this, link.toStdString(), [this](StoreActionResult r) {
    add_->setEnabled(true);
    if (!r.ok) return SetStatus("Could not add it: " + error_help::Describe(r.error), true);
    emit Changed();
    link_->clear();
    Refresh();
  });
}

}  // namespace mira_gui
