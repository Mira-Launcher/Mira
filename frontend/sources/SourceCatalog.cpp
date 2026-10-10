#include "SourceCatalog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "../settings/SettingsCard.h"
#include "../theme/Icons.h"
#include "../theme/Theme.h"
#include "SourceText.h"

namespace mira_gui {

SourceCatalog::SourceCatalog(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(12);

  search_ = new QLineEdit(this);
  search_->setPlaceholderText("Search sources");
  search_->setClearButtonEnabled(true);
  search_->setMinimumHeight(34);
  search_->addAction(icons::For(icons::Glyph::Search, theme::Current().text_muted), QLineEdit::LeadingPosition);
  connect(search_, &QLineEdit::textChanged, this, &SourceCatalog::ApplyFilter);
  layout->addWidget(search_);

  auto* chips = new QHBoxLayout();
  chips->setContentsMargins(0, 0, 0, 0);
  chips->setSpacing(6);
  all_ = new QPushButton("All", this);
  games_ = new QPushButton("Games", this);
  apps_ = new QPushButton("Apps", this);
  for (QPushButton* chip : {all_, games_, apps_}) {
    chip->setObjectName("chip");
    chip->setCheckable(true);
    chip->setAutoExclusive(true);
    chips->addWidget(chip);
  }
  all_->setChecked(true);
  // Connected once checked, so the filter never runs before the cards exist.
  for (QPushButton* chip : {all_, games_, apps_}) {
    connect(chip, &QPushButton::toggled, this, &SourceCatalog::ApplyFilter);
  }
  chips->addStretch(1);
  layout->addLayout(chips);

  games_card_ = new SettingsCard("Game sources", this);
  apps_card_ = new SettingsCard("App sources", this);
  layout->addWidget(games_card_);
  layout->addWidget(apps_card_);

  empty_ = new QLabel(this);
  empty_->setProperty("role", "subtle");
  empty_->setVisible(false);
  layout->addWidget(empty_);

  layout->addStretch(1);
}

void SourceCatalog::SetSources(const std::vector<SourceInfo>& sources) {
  sources_ = sources;
  games_card_->ClearRows();
  apps_card_->ClearRows();
  rows_.clear();
  haystack_.clear();
  for (const SourceInfo& source : sources) {
    const SourceCopy copy = CopyFor(source.id.toStdString());
    SettingsCard* card = copy.item == "app" ? apps_card_ : games_card_;
    auto* row = new SettingRow(source.name, copy.blurb, card);
    row->SetLeading(MakeSourceBadge(source, 24, row));
    row->AddAfterLabel(MakeKindTag(source, row));

    const QString id = source.id;
    auto* set_up = new QPushButton("Set up", row);
    set_up->setAccessibleName("Set up " + source.name);
    connect(set_up, &QPushButton::clicked, this, [this, id] { emit SetUpRequested(id); });
    row->AddControl(set_up);

    card->AddRow(row);
    rows_[id] = row;
    haystack_[id] = QStringList{source.name, KindLabel(source.kind), copy.blurb, copy.tool}.join(' ').toLower();
  }
  ApplyFilter();
}

void SourceCatalog::FocusSearch() {
  search_->setFocus(Qt::OtherFocusReason);
  search_->selectAll();
}

void SourceCatalog::ApplyFilter() {
  const QString typed = search_->text().trimmed();
  const QString q = typed.toLower();
  bool games_shown = false;
  bool apps_shown = false;
  for (const SourceInfo& source : sources_) {
    const bool app = CopyFor(source.id.toStdString()).item == "app";
    const bool shown = (q.isEmpty() || haystack_[source.id].contains(q)) &&
                       (all_->isChecked() || (games_->isChecked() && !app) || (apps_->isChecked() && app));
    rows_[source.id]->setVisible(shown);
    (app ? apps_shown : games_shown) |= shown;
  }
  games_card_->setVisible(games_shown);
  apps_card_->setVisible(apps_shown);

  if (games_shown || apps_shown) {
    empty_->setVisible(false);
    return;
  }
  if (sources_.empty()) {
    empty_->setText("Every source is set up.");
  } else if (q.isEmpty()) {
    empty_->setText("Nothing to add here.");
  } else {
    empty_->setText(QString("No source matches “%1”.").arg(typed));
  }
  empty_->setVisible(true);
}

}  // namespace mira_gui
