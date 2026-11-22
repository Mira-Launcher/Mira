#include "TagPicker.h"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <algorithm>

#include "../app/Notify.h"
#include "../client/api/Tags.h"
#include "../components/Card.h"
#include "../components/Labels.h"

namespace mira_gui {

TagPicker::TagPicker(ArtworkStore* artwork, QWidget* parent)
    : GamePicker(artwork, "Back to Tags", parent) {
  name_edit_ = new QLineEdit();
  name_edit_->setPlaceholderText("Tag name");
  name_edit_->setFixedWidth(240);
  connect(name_edit_, &QLineEdit::textChanged, this, &TagPicker::UpdateBar);
  AddHeaderWidget(name_edit_);

  folder_ = new Switch(BarBox());
  connect(folder_, &Switch::toggled, this, &TagPicker::UpdateBar);
  AddBarWidget(folder_);
  folder_label_ = new QLabel();
  folder_label_->setToolTip("A folder for it in each library folder sorted by tag (Tag settings)");
  AddBarWidget(folder_label_);
}

void TagPicker::Open(const QString& name, bool existing, std::vector<Section> sections,
                     const std::vector<GameSummary>& games, bool folder,
                     std::vector<TagSummary> known) {
  name_ = name;
  existing_ = existing;
  folder_was_ = folder;
  known_ = std::move(known);
  had_.clear();
  kept_.clear();
  std::vector<std::string> ticked;
  for (const Section& section : sections) {
    if (!section.ticked) continue;
    ticked.insert(ticked.end(), section.ids.begin(), section.ids.end());
    if (existing) had_.insert(had_.end(), section.ids.begin(), section.ids.end());
  }
  SetTitle(name.isEmpty() ? QString("New tag")
           : existing     ? QString("Choose games for %1").arg(name)
                          : QString("Add %1").arg(name));
  name_edit_->setVisible(name.isEmpty());
  name_edit_->clear();
  folder_->setChecked(folder);
  folder_label_->setText(existing ? "A folder" : "Also a folder");

  Clear();

  // A store's launcher isn't a game to tag; one that had the tag keeps it.
  std::vector<const GameSummary*> sorted;
  for (const GameSummary& game : games) {
    if (game.source == "launcher") {
      if (std::ranges::contains(had_, game.id)) kept_.push_back(game.id);
      continue;
    }
    sorted.push_back(&game);
  }
  std::vector<const GameSummary*> hidden;
  SortAndSplit(sorted, hidden);
  std::vector<std::string> placed;
  for (const Section& section : sections) {
    std::vector<const GameSummary*> in;
    for (const GameSummary* game : sorted) {
      if (std::ranges::contains(section.ids, game->id) &&
          !std::ranges::contains(placed, game->id)) {
        in.push_back(game);
        placed.push_back(game->id);
      }
    }
    // Its hidden games wait in the Hidden section, but count here, as on the Tags page.
    const auto hidden_in = std::ranges::count_if(hidden, [&](const GameSummary* game) {
      return std::ranges::contains(section.ids, game->id);
    });
    if (!in.empty()) {
      const QString count = hidden_in > 0 ? QString("%1 + %2 hidden").arg(in.size()).arg(hidden_in)
                                          : QString::number(in.size());
      AddSection(QString("%1 · %2").arg(section.heading, count), in,
                 section.ticked ? section.ids : std::vector<std::string>{});
    }
  }
  std::vector<const GameSummary*> rest;
  for (const GameSummary* game : sorted) {
    if (!std::ranges::contains(placed, game->id)) rest.push_back(game);
  }
  if (!rest.empty())
    AddSection(placed.empty() ? QString("Your games") : QString("Your other games"), rest, {});
  // Ticked as any other game would be, so the count matches the Tags page's even out of sight.
  if (!hidden.empty())
    AddSection(QString("Hidden · %1").arg(hidden.size()), hidden, ticked, /*hidden=*/true);
  FinishSections();
  if (name.isEmpty()) {
    name_edit_->setFocus();
  } else {
    Search()->setFocus();
  }
}

std::vector<std::string> TagPicker::Chosen() const {
  std::vector<std::string> ids = kept_;
  std::ranges::move(TickedIds(), std::back_inserter(ids));
  return ids;
}

void TagPicker::UpdateApply() {
  const int count = TickedCount();
  if (existing_) {
    std::vector<std::string> now = Chosen();
    std::vector<std::string> had = had_;
    std::ranges::sort(now);
    std::ranges::sort(had);
    ApplyButton()->setText(QString("Save, %1").arg(GamesCount(count)));
    ApplyButton()->setEnabled(now != had || folder_->isChecked() != folder_was_);
  } else {
    ApplyButton()->setText(QString("Add to %1").arg(GamesCount(count)));
    ApplyButton()->setEnabled(count > 0 &&
                              (!name_.isEmpty() || !name_edit_->text().trimmed().isEmpty()));
  }
}

void TagPicker::Apply() {
  const std::string name = (name_.isEmpty() ? name_edit_->text().trimmed() : name_).toStdString();
  if (name.empty()) return;
  std::vector<std::string> ids = Chosen();
  // A new name that's already a tag adds to it rather than taking it off the games that have it.
  if (!existing_) {
    const auto known =
        std::ranges::find_if(known_, [&](const TagSummary& t) { return SameTag(t.name, name); });
    if (known != known_.end()) {
      for (const std::string& id : known->ids) {
        if (!std::ranges::contains(ids, id)) ids.push_back(id);
      }
    }
  }
  const std::optional<bool> folder = folder_->isChecked() != folder_was_
                                         ? std::optional<bool>(folder_->isChecked())
                                         : std::nullopt;
  ApplyButton()->setEnabled(false);
  api::SetTagAsync(this, name, ids, folder, [this](PatchGamesResult result) {
    if (!result.ok) {
      UpdateBar();
      notify::FailedRequest(window(), "Could not change that tag", result.error);
      return;
    }
    emit Applied(result.games);
    emit Closed();
  });
}

}  // namespace mira_gui
