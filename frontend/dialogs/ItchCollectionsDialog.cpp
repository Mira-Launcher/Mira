#include "ItchCollectionsDialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "../client/MiradClient.h"
#include "../app/ErrorHelp.h"

ItchCollectionsDialog::ItchCollectionsDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle("itch.io collections");
  setMinimumWidth(480);
  auto* layout = new QVBoxLayout(this);

  auto* intro = new QLabel(
      "Games in these collections show on the itch page. Free ones can be installed; paid ones "
      "install once you own them.",
      this);
  intro->setProperty("role", "muted");
  intro->setWordWrap(true);
  layout->addWidget(intro);

  list_ = new QVBoxLayout();
  list_->setSpacing(4);
  layout->addLayout(list_);

  status_ = new QLabel(this);
  status_->setProperty("role", "muted");
  status_->setWordWrap(true);
  layout->addWidget(status_);

  auto* add_row = new QHBoxLayout();
  link_ = new QLineEdit(this);
  link_->setPlaceholderText("https://itch.io/c/123456/collection-name");
  connect(link_, &QLineEdit::returnPressed, this, &ItchCollectionsDialog::Add);
  add_row->addWidget(link_, /*stretch=*/1);
  add_ = new QPushButton("Add", this);
  connect(add_, &QPushButton::clicked, this, &ItchCollectionsDialog::Add);
  add_row->addWidget(add_);
  layout->addLayout(add_row);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  Refresh();
}

void ItchCollectionsDialog::Refresh() {
  status_->setText("Loading…");
  mira_gui::MiradClient::GetItchCollectionsAsync(
      this, [this](mira_gui::ItchCollectionsResult result) { ShowCollections(result); });
}

void ItchCollectionsDialog::ShowCollections(const mira_gui::ItchCollectionsResult& result) {
  while (QLayoutItem* item = list_->takeAt(0)) {
    if (item->widget() != nullptr) item->widget()->deleteLater();
    delete item;
  }
  if (!result.ok) {
    status_->setText("Could not list the collections: " + mira_gui::error_help::Describe(result.error));
    return;
  }
  status_->setText(result.collections.empty() ? "No collections yet." : QString());
  for (const mira_gui::ItchCollection& collection : result.collections) {
    auto* row = new QWidget(this);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    auto* name = new QLabel(QString("%1  ·  %2 game%3")
                                .arg(QString::fromStdString(collection.title))
                                .arg(collection.games_count)
                                .arg(collection.games_count == 1 ? "" : "s"),
                            row);
    row_layout->addWidget(name, /*stretch=*/1);
    if (collection.own) {
      auto* yours = new QLabel("Yours", row);
      yours->setProperty("role", "muted");
      row_layout->addWidget(yours);
    } else {
      auto* remove = new QPushButton("Remove", row);
      const std::int64_t id = collection.id;
      connect(remove, &QPushButton::clicked, this, [this, id] {
        mira_gui::MiradClient::RemoveItchCollectionAsync(
            this, id, [this](mira_gui::StoreActionResult r) {
              if (!r.ok) {
                status_->setText("Could not remove it: " + mira_gui::error_help::Describe(r.error));
                return;
              }
              changed_ = true;
              Refresh();
            });
      });
      row_layout->addWidget(remove);
    }
    list_->addWidget(row);
  }
}

void ItchCollectionsDialog::Add() {
  const QString link = link_->text().trimmed();
  if (link.isEmpty()) return;
  add_->setEnabled(false);
  status_->setText("Adding…");
  mira_gui::MiradClient::AddItchCollectionAsync(
      this, link.toStdString(), [this](mira_gui::StoreActionResult r) {
        add_->setEnabled(true);
        if (!r.ok) {
          status_->setText("Could not add it: " + mira_gui::error_help::Describe(r.error));
          return;
        }
        changed_ = true;
        link_->clear();
        Refresh();
      });
}
