#include "OverridesEditor.h"

#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

#include "../ui/Icons.h"
#include "../ui/Notify.h"
#include "../ui/SettingsCard.h"
#include "../ui/SettingsNav.h"
#include "../ui/Theme.h"

#include <algorithm>
#include <utility>

#include "../client/EventHub.h"
#include "../client/MiradClient.h"

namespace mira_gui {

OverridesEditor::OverridesEditor(std::string game_id, QWidget* parent)
    : QWidget(parent), game_id_(std::move(game_id)) {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);

  nav_ = new SettingsNavWidget(this);
  // Inside a game's card, where each row also carries its layer and Clear.
  nav_->SetNavWidth(170);
  outer->addWidget(nav_);
}

void OverridesEditor::Load() {
  MiradClient::GetConfigSchemaAsync(this, [this](ConfigSchemaResult schema) {
    if (!schema.ok) return;  // non-fatal: the main game fields still work without this section
    BuildRows(schema);
    Reload();
  });
}

void OverridesEditor::Reload() {
  MiradClient::GetGameConfigAsync(this, game_id_, [this](GameConfigResult config) {
    if (config.ok) ApplyValues(config);
  });
}

void OverridesEditor::BuildRows(const ConfigSchemaResult& schema) {
  // Only what the schema marks per-game, in the same order and categories as
  // the main Settings screen.
  std::vector<ConfigSchemaEntry> entries;
  for (const ConfigSchemaEntry& entry : schema.entries) {
    if (entry.per_game) entries.push_back(entry);
  }
  std::vector<std::string> categories;
  for (const ConfigSchemaEntry& entry : entries) categories.push_back(entry.category);

  // Every row is built before any is stored, so the vector never reallocates
  // under the lambdas below, which hold indices.
  fields_.reserve(entries.size());
  for (const auto& [category, rows] : GroupByCategory(categories)) {
    SettingsPage* page = nav_->AddCategory(category, CategoryGlyph(category));
    SettingsCard* card = nullptr;
    int group = -1;

    for (const size_t i : rows) {
      const ConfigSchemaEntry& entry = entries[i];
      if (card == nullptr || entry.group != group) {
        group = entry.group;
        card = page->AddCard(QString::fromStdString(entry.group_label));
      }

      fields_.emplace_back();
      Field& field = fields_.back();
      field.entry = entry;
      // The settings screen's own editor, so an enum is a dropdown and a runner a picker here too.
      const std::string& doc = entry.game_doc.empty() ? entry.doc : entry.game_doc;
      SettingRow* row = field.Build(card, QString::fromStdString(doc));
      field.layer_label = new QLabel(row);
      field.layer_label->setProperty("role", "subtle");
      row->AddAfterLabel(field.layer_label);
      field.clear = new QToolButton(row);
      field.clear->setAutoRaise(true);
      field.clear->setIcon(icons::For(icons::Glyph::Close, theme::Current().text_muted));
      field.clear->setToolTip("Use the global setting again");
      field.clear->hide();
      row->AddAfterLabel(field.clear);
      card->AddRow(row);

      const size_t index = fields_.size() - 1;
      connect(field.clear, &QToolButton::clicked, this, [this, index] { ResetField(index); });
      connect(row, &SettingRow::RevertClicked, this, [this, index] {
        fields_[index].SetText(fields_[index].original);
        fields_[index].row->SetModified(false);
        emit Changed();
      });
      field.OnEdited(this, [this, index] {
        fields_[index].row->SetModified(fields_[index].Changed());
        emit Changed();
      });
      nav_->RegisterRow(row, field.SearchText());
    }
  }
  const auto list_runners = [this] {
    MiradClient::ListRunnersAsync(this, [this](RunnersResult runners) {
      for (Field& field : fields_) {
        if (field.combo != nullptr && field.entry.is_runner_ref) FillRunnerCombo(field.combo, runners);
      }
    });
  };
  list_runners();
  connect(EventHub::Instance(), &EventHub::RunnersChanged, this, list_runners);
}

void OverridesEditor::ApplyValues(const GameConfigResult& config) {
  // BuildRows makes one row per schema key, since only this per-game
  // response says which are overridable, so daemon-only keys are hidden here
  // rather than never built.
  for (const GameConfigEntry& entry : config.entries) {
    const auto it = std::ranges::find(fields_, entry.key,
                                      [](const Field& f) { return f.entry.key; });
    if (it == fields_.end()) continue;
    Field& field = *it;

    nav_->SetRowGateVisible(field.row, entry.overridable);
    if (!entry.overridable) continue;

    // A reload after resetting one override keeps unsaved edits to the others.
    const bool pending = !field.layer.empty() && field.Text() != field.original && field.entry.key != resetting_key_;
    const std::string kept = field.Text();
    field.layer = entry.layer;
    field.layer_label->setText(entry.layer == "game"     ? QString("This game")
                               : entry.layer == "config" ? QString("From settings")
                                                         : QString("Default"));
    field.clear->setVisible(entry.layer == "game");
    field.SetText(entry.value_display);
    field.original = field.Text();  // as the editor holds it (a clamped number, a joined list)
    if (pending) field.SetText(kept);
    field.row->SetModified(field.Text() != field.original);
  }
  // The first values give lists and paths their rows; later reloads keep the columns put.
  if (!values_loaded_) nav_->RearrangePages();
  values_loaded_ = true;
  resetting_key_.clear();
  emit Changed();  // the change count, after a reset dropped that row's edit
}

std::vector<GameConfigEdit> OverridesEditor::PendingEdits() const {
  std::vector<GameConfigEdit> edits;
  for (const Field& field : fields_) {
    // An empty layer means ApplyValues never reached this row (not
    // overridable, or the fetch failed), so there is nothing to compare
    // against and nothing to send.
    const std::string current = field.Text();
    if (field.layer.empty() || current == field.original) continue;
    edits.push_back(GameConfigEdit{field.entry.key, field.entry.type, current, false});
  }
  return edits;
}

void OverridesEditor::MarkSaved() {
  for (Field& field : fields_) {
    field.original = field.Text();
    if (field.row != nullptr) field.row->SetModified(false);
  }
  Reload();  // each saved row now says "This game" and can be reset
}

void OverridesEditor::DiscardChanges() {
  for (Field& field : fields_) {
    if (field.row == nullptr || !field.Changed()) continue;
    field.SetText(field.original);
    field.row->SetModified(false);
  }
  emit Changed();
}

SettingsPage* OverridesEditor::AddPage(const QString& title, icons::Glyph glyph) {
  return nav_->AddCategory(title, glyph);
}

void OverridesEditor::ShowFromTop() {
  nav_->RearrangePages();
  nav_->ScrollToTop();
}

void OverridesEditor::ResetField(size_t index) {
  const Field& field = fields_[index];
  resetting_key_ = field.entry.key;
  MiradClient::PatchGameConfigAsync(
      this, game_id_, {GameConfigEdit{field.entry.key, field.entry.type, std::string(), true}},
      [this](PatchGameConfigResult result) {
        if (!result.ok) {
          notify::FailedRequest(this, "Could not reset this override.", result.error);
          return;
        }
        Reload();
      });
}

}  // namespace mira_gui
