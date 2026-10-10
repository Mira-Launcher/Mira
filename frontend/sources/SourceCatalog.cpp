#include "SourceCatalog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "../widgets/Labels.h"
#include "../widgets/TileGridLayout.h"
#include "SourceText.h"
#include "SourceTile.h"

namespace mira_gui {

namespace {

constexpr int kTileWidth = 280;
const QStringList kGroups = {"Stores", "Launchers", "Apps", "On this computer"};

int GroupOf(const SourceInfo& source) {
  if (CopyFor(source.id.toStdString()).item == "app") return 2;
  switch (source.kind) {
    case SourceInfo::Kind::Store:
      return 0;
    case SourceInfo::Kind::Launcher:
      return 1;
    case SourceInfo::Kind::Local:
      return 3;
  }
  return 3;
}

// What setting it up takes, in a few words.
QString Needs(const SourceInfo& source) {
  switch (source.kind) {
    case SourceInfo::Kind::Store:
      return "Sign in to your account";
    case SourceInfo::Kind::Launcher:
      return "Mira installs its launcher";
    case SourceInfo::Kind::Local:
      return "Reads what's on this computer";
  }
  return {};
}

}  // namespace

SourceCatalog::SourceCatalog(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(8);

  auto* chips = new QHBoxLayout();
  chips->setContentsMargins(0, 0, 0, 4);
  chips->setSpacing(6);
  all_ = new QPushButton(this);
  for (const QString& name : kGroups) {
    Group& group = groups_.emplace_back();
    group.name = name;
    group.chip = new QPushButton(this);
    group.heading = MakeGroupHeading(this, name);
    group.heading->setContentsMargins(0, 8, 0, 0);
    group.grid = new QWidget(this);
    new TileGridLayout(group.grid, kTileWidth);
  }
  QList<QPushButton*> buttons{all_};
  for (const Group& group : groups_) buttons << group.chip;
  for (QPushButton* chip : buttons) {
    chip->setObjectName("chip");
    chip->setCheckable(true);
    chip->setAutoExclusive(true);
    chips->addWidget(chip);
  }
  all_->setChecked(true);
  // Connected once checked, so the filter never runs before the tiles exist.
  for (QPushButton* chip : buttons) connect(chip, &QPushButton::toggled, this, &SourceCatalog::ApplyFilter);
  chips->addStretch(1);
  layout->addLayout(chips);

  for (const Group& group : groups_) {
    layout->addWidget(group.heading);
    layout->addWidget(group.grid);
  }
  empty_ = MakeLabel(this, QString(), "subtle");
  empty_->hide();
  layout->addWidget(empty_);
  layout->addStretch(1);
}

void SourceCatalog::SetSources(const std::vector<SourceInfo>& sources) {
  for (auto& [id, tile] : tiles_) delete tile;
  tiles_.clear();
  group_of_.clear();
  haystack_.clear();
  std::vector<int> counts(groups_.size(), 0);
  for (const SourceInfo& source : sources) {
    const QString id = source.id;
    const SourceCopy copy = CopyFor(id.toStdString());
    const int group = GroupOf(source);
    ++counts[group];
    auto* tile = new SourceTile(source, groups_[group].grid);
    tile->Line()->setText(Needs(source));
    QLabel* blurb = MakeLabel(tile, copy.blurb, "subtle");
    blurb->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    tile->Body()->addWidget(blurb, 1);
    // Lutris has nothing to set up: its button imports right away.
    const bool imports = id == "lutris";
    auto* button = new QPushButton(imports ? "Import games" : "Set up…", tile);
    button->setDefault(true);
    button->setAccessibleName((imports ? "Import games from " : "Set up ") + source.name);
    button->setToolTip(imports ? QString("Adds %1's games now. They stay playable in %1.").arg(source.name)
                               : QString("Walks you through adding %1.").arg(source.name));
    connect(button, &QPushButton::clicked, this, [this, id] { emit SetUpRequested(id); });
    auto* footer = new QHBoxLayout();
    footer->addStretch(1);
    footer->addWidget(button);
    tile->Body()->addLayout(footer);
    groups_[group].grid->layout()->addWidget(tile);
    tiles_[id] = tile;
    group_of_[id] = group;
    haystack_[id] = QStringList{source.name, KindLabel(source.kind), kGroups[group], copy.blurb, copy.tool}.join(' ').toLower();
  }
  int total = 0;
  for (size_t i = 0; i < groups_.size(); ++i) {
    groups_[i].chip->setText(QString("%1  %2").arg(groups_[i].name).arg(counts[i]));
    groups_[i].chip->setVisible(counts[i] > 0);
    total += counts[i];
  }
  all_->setText(QString("All  %1").arg(total));
  if (!all_->isChecked() && std::ranges::none_of(groups_, [](const Group& g) { return g.chip->isChecked() && g.chip->isVisible(); })) {
    all_->setChecked(true);
  }
  ApplyFilter();
}

void SourceCatalog::SetFilter(const QString& text) {
  filter_ = text.trimmed().toLower();
  ApplyFilter();
}

void SourceCatalog::ApplyFilter() {
  std::vector<bool> shown_in(groups_.size(), false);
  for (const auto& [id, tile] : tiles_) {
    const int group = group_of_[id];
    const bool shown = (filter_.isEmpty() || haystack_[id].contains(filter_)) &&
                       (all_->isChecked() || groups_[group].chip->isChecked());
    tile->setVisible(shown);
    if (shown) shown_in[group] = true;
  }
  bool any = false;
  for (size_t i = 0; i < groups_.size(); ++i) {
    groups_[i].heading->setVisible(shown_in[i]);
    groups_[i].grid->setVisible(shown_in[i]);
    any = any || shown_in[i];
  }
  empty_->setVisible(!any);
  if (tiles_.empty()) {
    empty_->setText("Every source is set up.");
  } else if (!filter_.isEmpty()) {
    empty_->setText("No source matches that. It may already be under Added.");
  } else {
    empty_->setText("Nothing to add here.");
  }
}

}  // namespace mira_gui
