#include "OverridesEditor.h"

#include <QLabel>
#include <QPushButton>
#include <QWidget>
#include <algorithm>
#include <utility>

#include "../client/EventHub.h"
#include "../client/api/Config.h"
#include "../client/api/Games.h"
#include "../client/api/Runners.h"
#include "../settings/SettingsCard.h"
#include "../widgets/ChipToggles.h"
#include "../widgets/ListEdit.h"

namespace mira_gui {

OverridesEditor::OverridesEditor(std::string game_id, QWidget* parent)
    : QObject(parent), game_id_(std::move(game_id)), parent_(parent) {}

void OverridesEditor::PlaceRow(const std::string& key, SettingsCard* card, int at) {
  placed_[key] = Placement{card, at};
}

void OverridesEditor::SetOverridesCard(SettingsCard* card) {
  overrides_card_ = card;
  card->hide();  // until a category has a row this game can use
}

void OverridesEditor::Load() {
  // Rows are built once; a reload only refreshes their values.
  if (!fields_.empty()) return Reload();
  api::GetConfigSchemaAsync(this, [this](ConfigSchemaResult schema) {
    if (!schema.ok || !fields_.empty()) return;  // or an earlier Load's reply built them
    BuildRows(schema);
    Reload();
  });
}

void OverridesEditor::Reload() {
  // Settings' values too, which undo puts back and may have changed since.
  api::GetConfigAsync(this, [this](ConfigResult settings) {
    if (settings.ok) settings_values_ = settings.values;
    api::GetGameConfigAsync(this, game_id_, [this](GameConfigResult config) {
      if (config.ok) ApplyValues(config);
    });
  });
}

void OverridesEditor::BuildRows(const ConfigSchemaResult& schema) {
  size_t count = 0;
  for (const ConfigSchemaEntry& entry : schema.entries) count += entry.per_game;
  // Reserved up front: the lambdas below hold indices, and the rows hold no pointers into it.
  fields_.reserve(count);
  for (const ConfigSchemaEntry& entry : schema.entries) {
    if (!entry.per_game) continue;
    fields_.emplace_back();
    Field& field = fields_.back();
    field.entry = entry;
    // A setting shown inside another's row goes where that one does.
    auto placed = placed_.find(entry.key);
    if (placed == placed_.end() && !entry.shown_in.empty()) placed = placed_.find(entry.shown_in);
    QWidget* parent = placed != placed_.end() ? static_cast<QWidget*>(placed->second.card)
                                              : static_cast<QWidget*>(overrides_card_);
    const std::string& doc = entry.game_doc.empty() ? entry.doc : entry.game_doc;
    SettingRow* row = field.Build(parent, QString::fromStdString(doc));
    row->hide();  // until this game's values say it can use it
    if (placed != placed_.end()) {
      SettingsCard* card = placed->second.card;
      card->AddRow(row);
      if (placed->second.at >= 0) card->MoveRow(row, placed->second.at);
      field.card = card;
    } else if (overrides_card_ != nullptr) {
      ExpanderRow*& expander = expanders_[entry.category];
      if (expander == nullptr) {
        expander = new ExpanderRow(QString::fromStdString(entry.category), overrides_card_);
        expander->hide();
        overrides_card_->AddRow(expander);
      }
      expander->AddRow(row);
      field.expander = expander;
    }
    const size_t index = fields_.size() - 1;
    if (field.list != nullptr) {
      // A list the game takes from Settings shows it muted; Edit… starts the game's own copy.
      field.from_settings = new QLabel("From Settings", row);
      field.from_settings->setProperty("role", "subtle");
      row->AddAfterLabel(field.from_settings);
      field.edit_list = new QPushButton("Edit…", row);
      field.edit_list->setToolTip("Give this game a list of its own, starting from Settings' list");
      row->AddControl(field.edit_list);
      connect(field.edit_list, &QPushButton::clicked, this, [this, index] {
        Field& f = fields_[index];
        f.unlocked = true;
        ShowState(f);
        f.list->AddButton()->click();
      });
    }
    if (entry.shown_in.empty())  // else its host's undo covers it
      connect(row, &SettingRow::RevertClicked, this, [this, index] { Undo(index); });
    field.OnEdited(this, [this, index] {
      if (loading_) return;
      Field& f = fields_[index];
      if (f.clearing && f.Text() != f.settings) f.clearing = false;  // edited again: a new value
      ShowState(f);
      UpdateSummaries();
      emit Changed();
    });
  }
  // A script's ready-made steps sit in its row, and a step turned on or off before the game turns
  // its pair on or off after it, as in Settings.
  for (size_t i = 0; i < fields_.size(); ++i) {
    Field& field = fields_[i];
    Field* host = Find(field.entry.shown_in);
    if (host != nullptr && host->line != nullptr && field.chips != nullptr) {
      host->Embed(field);
      field.embedded = true;
      field.expander = host->expander;
      field.card = host->card;
    }
    // Once, from the later of the two.
    const Field* paired = Find(field.entry.paired_key);
    if (paired != nullptr && paired < &field) PairChoices(*paired, field);
  }
  const auto list_runners = [this] {
    api::ListRunnersAsync(this, [this](RunnersResult runners) {
      loading_ = true;
      for (Field& field : fields_) {
        if (field.combo != nullptr && field.entry.is_runner_ref)
          FillRunnerCombo(field.combo, runners, field.entry);
      }
      loading_ = false;
    });
  };
  list_runners();
  connect(EventHub::Instance(), &EventHub::RunnersChanged, this, list_runners);
}

void OverridesEditor::ApplyValues(const GameConfigResult& config) {
  loading_ = true;
  for (const GameConfigEntry& entry : config.entries) {
    Field* found = Find(entry.key);
    if (found == nullptr) continue;
    Field& field = *found;
    field.applies = entry.overridable && entry.applies;
    if (!entry.overridable) continue;

    // A reload after a save elsewhere keeps unsaved edits.
    const bool pending = !field.layer.empty() && (field.Changed() || field.clearing);
    const std::string kept = field.Text();
    const bool kept_clearing = field.clearing;
    field.layer = entry.layer;
    if (entry.layer == "game") {
      const auto settings = settings_values_.find(entry.key);
      field.SetText(settings != settings_values_.end() ? settings->second
                                                       : field.entry.default_display);
      field.settings = field.Text();  // as the editor holds it (a clamped number, a joined list)
    }
    field.SetText(entry.value_display);
    field.original = field.Text();
    if (entry.layer != "game") field.settings = field.original;
    field.clearing = pending && kept_clearing && entry.layer == "game";
    if (pending) field.SetText(kept);
    ShowState(field);
  }
  loading_ = false;
  values_loaded_ = true;
  UpdateSummaries();
  RevealKey(std::exchange(reveal_key_, {}));
  emit Changed();
}

bool OverridesEditor::Own(const Field& field) const {
  return !field.clearing && (field.layer == "game" || field.Changed());
}

void OverridesEditor::ShowState(Field& field) {
  if (field.embedded)
    field.chips->setVisible(field.applies && !field.hidden);
  else
    field.row->setVisible(field.applies && !field.hidden);
  // The dot only for an edit not saved yet; a saved override keeps just the undo to Settings'.
  field.row->SetModified(field.Changed() || field.clearing);
  field.row->SetOverridden(field.layer == "game" && !field.clearing);
  if (field.list == nullptr) return;
  const bool inherited = !Own(field) && !field.unlocked;
  field.list->SetReadOnly(inherited);
  field.list->AddButton()->setVisible(!inherited);
  field.edit_list->setVisible(inherited);
  field.from_settings->setVisible(inherited && !field.list->Items().isEmpty());
}

void OverridesEditor::Undo(size_t index) {
  std::vector<Field*> group = {&fields_[index]};
  for (Field& shown : fields_) {
    if (shown.embedded && shown.entry.shown_in == fields_[index].entry.key) group.push_back(&shown);
  }
  const bool edited = std::ranges::any_of(
      group, [](const Field* field) { return field->Changed() || field->clearing; });
  loading_ = true;
  for (Field* field : group) {
    if (edited) {
      field->clearing = false;  // back to the game's own value, or Settings' when it has none
      field->SetText(field->original);
    } else if (field->layer == "game") {
      field->clearing = true;
      field->SetText(field->settings);
    }
  }
  loading_ = false;
  for (Field* field : group) {
    field->unlocked = false;
    ShowState(*field);
  }
  UpdateSummaries();
  emit Changed();
}

const OverridesEditor::Field* OverridesEditor::Find(const std::string& key) const {
  if (key.empty()) return nullptr;
  const auto it = std::ranges::find(fields_, key, [](const Field& f) { return f.entry.key; });
  return it != fields_.end() ? &*it : nullptr;
}

OverridesEditor::Field* OverridesEditor::Find(const std::string& key) {
  return const_cast<Field*>(std::as_const(*this).Find(key));
}

void OverridesEditor::HideWhenEmpty(SettingsCard* card) {
  hide_when_empty_.push_back(card);
  card->hide();  // until one of its rows is one this game can use
}

void OverridesEditor::UpdateSummaries() {
  bool any = false;
  for (const auto& [category, expander] : expanders_) {
    int shown = 0;
    int own = 0;
    for (const Field& field : fields_) {
      if (field.expander != expander || !field.applies || field.hidden) continue;
      ++shown;
      own += Own(field);
    }
    expander->setVisible(shown > 0);
    expander->SetSummary(own > 0 ? QString("%1 changed").arg(own) : QString("As in Settings"));
    any = any || shown > 0;
  }
  if (overrides_card_ != nullptr) overrides_card_->setVisible(any);
  for (SettingsCard* card : hide_when_empty_) {
    card->setVisible(std::ranges::any_of(fields_, [card](const Field& field) {
      return field.card == card && !field.embedded && field.applies && !field.hidden;
    }));
  }
}

std::string OverridesEditor::Text(const std::string& key) const {
  const Field* field = Find(key);
  return field == nullptr ? std::string() : field->Text();
}

void OverridesEditor::SetRowHidden(const std::string& key, bool hidden) {
  Field* field = Find(key);
  if (field == nullptr || field->hidden == hidden) return;
  field->hidden = hidden;
  if (values_loaded_) ShowState(*field);
  UpdateSummaries();
}

void OverridesEditor::RevealKey(const std::string& key) {
  if (key.empty()) return;
  const Field* field = Find(key);
  // Before the values arrive the rows are hidden, so it waits for them.
  if (!values_loaded_ || field == nullptr) {
    reveal_key_ = key;
    return;
  }
  if (field->expander != nullptr) field->expander->SetExpanded(true);
  const Field* host = field->embedded ? Find(field->entry.shown_in) : nullptr;
  emit RevealRequested((host != nullptr ? host : field)->row);
}

std::vector<GameConfigEdit> OverridesEditor::PendingEdits() const {
  std::vector<GameConfigEdit> edits;
  for (const Field& field : fields_) {
    // An empty layer means ApplyValues never reached this row (not overridable, or the fetch
    // failed), so there is nothing to compare against and nothing to send.
    if (field.layer.empty()) continue;
    if (field.clearing) {
      edits.push_back(GameConfigEdit{field.entry.key, field.entry.type, std::string(), true});
    } else if (field.Changed()) {
      edits.push_back(GameConfigEdit{field.entry.key, field.entry.type, field.Text(), false});
    }
  }
  return edits;
}

void OverridesEditor::MarkSaved() {
  for (Field& field : fields_) {
    if (field.layer.empty()) continue;
    if (field.clearing) {
      field.layer = "config";
      field.clearing = false;
    } else if (field.Changed()) {
      field.layer = "game";
    }
    field.original = field.Text();
    field.unlocked = false;
    ShowState(field);
  }
  UpdateSummaries();
  Reload();  // the layers as mirad now resolves them
}

void OverridesEditor::DiscardChanges() {
  loading_ = true;
  for (Field& field : fields_) {
    if (field.layer.empty()) continue;
    field.clearing = false;
    field.unlocked = false;
    if (field.Changed()) field.SetText(field.original);
    ShowState(field);
  }
  loading_ = false;
  UpdateSummaries();
  emit Changed();
}

}  // namespace mira_gui
